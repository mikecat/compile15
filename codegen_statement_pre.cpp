#include <string>
#include "ast.h"
#include "codegen.hpp"
#include "codegen_internal.hpp"

// 今のブロックに変数を登録し、登録した変数のオフセットを返す
int codegen_register_variable(ast_node* def_node, codegen_status& status,
bool argument_mode, bool pragma_use_register, int pragma_use_register_id) {
	if (def_node == nullptr || def_node->kind != (argument_mode ? NODE_ARGUMENT : NODE_VAR_DEFINE)) {
		throw codegen_error(def_node == nullptr ? 0 : def_node->lineno,
			std::string("tried to register something not ") + (argument_mode ? "an argument" : "a variable"));
	}
	auto& mem_offset = status.lv_mem_offset.back();
	auto& reg_offset = status.lv_reg_offset.back();
	auto& var_map = status.var_maps.back();
	type_node* type = argument_mode ? def_node->d.arg.type : def_node->d.var_def.type;
	char* name = argument_mode ? def_node->d.arg.name : def_node->d.var_def.name;
	int is_register = argument_mode ? def_node->d.arg.is_register : def_node->d.var_def.is_register;
	if (var_map.find(name) != var_map.end()) {
		throw codegen_error(def_node->lineno,
			std::string("multiple definition of ") + (argument_mode ? "argument " : "variable ") + name);
	}
	int reg_specify = -1;
	if (argument_mode) {
		if (def_node->d.arg.pragmas != nullptr && def_node->d.arg.pragmas->kind == NODE_ARRAY) {
			size_t pragma_node_num = def_node->d.arg.pragmas->d.array.num;
			ast_node** pragma_nodes = def_node->d.arg.pragmas->d.array.nodes;
			for (size_t i = 0; i < pragma_node_num; i++) {
				if (pragma_nodes[i]->kind == NODE_PRAGMA) {
					size_t token_num = pragma_nodes[i]->d.array.num;
					ast_node** tokens = pragma_nodes[i]->d.array.nodes;
					if (token_num >= 1 && tokens[0]->kind == NODE_CONTROL_IDENTIFIER &&
					std::string(tokens[0]->d.identifier.name) == "use_register") {
						is_register = 1;
						def_node->d.arg.is_register = 1;
						if (token_num >= 2 && tokens[1]->kind == NODE_CONTROL_INTEGER) {
							if (reg_specify >= 0) {
								throw codegen_error(def_node->lineno, "multiple register specification");
							}
							reg_specify = tokens[1]->d.integer.value;
						}
					}
				}
			}
		}
	} else {
		if (pragma_use_register) {
			is_register = 1;
			def_node->d.var_def.is_register = 1;
			if (pragma_use_register_id >= 0) reg_specify = pragma_use_register_id;
		}
	}
	var_info* vi;
	int offset;
	if (is_register) {
		offset = reg_offset;
		vi = new var_info(reg_offset, type, false, true);
		reg_offset++;
		if (status.lv_reg_size < reg_offset) {
			status.lv_reg_size = reg_offset;
			status.lv_reg_assign.resize(status.lv_reg_size, -1);
		}
		if (reg_specify >= 0) {
			if (status.lv_reg_assign[offset] >= 0 && status.lv_reg_assign[offset] != reg_specify) {
				throw codegen_error(def_node->lineno, "register specification conflict");
			}
			status.lv_reg_assign[offset] = reg_specify;
		}
	} else {
		if (mem_offset % type->align != 0) {
			mem_offset += type->align - (mem_offset % type->align);
		}
		offset = mem_offset;
		vi = new var_info(mem_offset, type, false, false);
		mem_offset += argument_mode ? 4 : type->size;
		if (status.lv_mem_size < mem_offset) status.lv_mem_size = mem_offset;
	}
	var_map[name] = vi;
	if (!argument_mode) def_node->d.var_def.info = vi;
	return offset;
}

// 文で用いられる式の前処理を行う
// * 識別子を解決する
// * constfoldをする
// * トップレベルに演算子の自動挿入を行う
// * グローバル変数および関数呼び出しがあるかを調べる
void codegen_preprocess_statement_expr(expression_node** expr, int lineno, codegen_status& status) {
	codegen_resolve_identifier_expr(*expr, lineno, status);
	*expr = constfold(*expr);
	codegen_add_auto_operator(OP_NONE, 0, expr);
	codegen_preprocess_expr(*expr, lineno, status);
}

// 文の前処理を行う
// * 変数に割り当てるレジスタの指定を読み取る
// * 識別子を登録する
// * 式の前処理を行う
void codegen_preprocess_statement(ast_node* ast, codegen_status& status) {
	if (ast == nullptr) {
		throw codegen_error(0, "NULL passed to codegen_preprocess_statement()");
	}
	switch (ast->kind) {
	case NODE_ARRAY:
		// このブロック用の情報を作る
		status.lv_mem_offset.push_back(status.lv_mem_offset.back());
		status.lv_reg_offset.push_back(status.lv_reg_offset.back());
		status.var_maps.push_back(std::map<std::string, var_info*>());

		// このブロックの中身を処理する
		status.pragma_use_register = false;
		status.pragma_use_register_id = -1;
		for (size_t i = 0; i < ast->d.array.num; i++) {
			codegen_preprocess_statement(ast->d.array.nodes[i], status);
			if (ast->d.array.nodes[i]->kind != NODE_PRAGMA) {
				status.pragma_use_register = false;
				status.pragma_use_register_id = -1;
			}
		}

		// このブロック用の情報を破棄する
		status.lv_mem_offset.pop_back();
		status.lv_reg_offset.pop_back();
		status.var_maps.pop_back();
		break;
	case NODE_VAR_DEFINE:
		codegen_register_variable(ast, status, false, status.pragma_use_register, status.pragma_use_register_id);
		if (ast->d.var_def.initializer != nullptr) {
			codegen_preprocess_statement_expr(&ast->d.var_def.initializer, ast->lineno, status);
		}
		break;
	case NODE_FUNC_DEFINE:
		throw codegen_error(ast->lineno, "cannot define function inside function");
		break;
	case NODE_EXPR:
		codegen_preprocess_statement_expr(&ast->d.expr.expression, ast->lineno, status);
		break;
	case NODE_EMPTY:
		// 何もしない
		break;
	case NODE_PRAGMA:
		{
			size_t token_num = ast->d.array.num;
			ast_node** tokens = ast->d.array.nodes;
			if (token_num >= 1 && tokens[0]->kind == NODE_CONTROL_IDENTIFIER &&
			std::string(tokens[0]->d.identifier.name) == "use_register") {
				status.pragma_use_register = true;
				if (token_num >= 2 && tokens[1]->kind == NODE_CONTROL_INTEGER) {
					if (status.pragma_use_register_id >= 0) {
						throw codegen_error(ast->lineno, "multiple register specification");
					}
					status.pragma_use_register_id = tokens[1]->d.integer.value;
				}
			}
		}
		break;
	case NODE_LABEL:
		if (status.goto_labels.find(ast->d.label.name) != status.goto_labels.end()) {
			throw codegen_error(ast->lineno, std::string("duplicate label ") + ast->d.label.name);
		}
		status.goto_labels[ast->d.label.name] = status.next_label++;
		codegen_preprocess_statement(ast->d.label.statement, status);
		break;
	case NODE_IF:
		codegen_preprocess_statement_expr(&ast->d.if_d.cond, ast->lineno, status);
		codegen_preprocess_statement(ast->d.if_d.true_statement, status);
		if (ast->d.if_d.false_statement != nullptr) {
			codegen_preprocess_statement(ast->d.if_d.false_statement, status);
		}
		break;
	case NODE_WHILE:
	case NODE_DO_WHILE:
		codegen_preprocess_statement_expr(&ast->d.while_d.cond, ast->lineno, status);
		codegen_preprocess_statement(ast->d.while_d.statement, status);
		break;
	case NODE_FOR:
		// 初期化での変数宣言用に、仮想的にブロックを作る
		status.lv_mem_offset.push_back(status.lv_mem_offset.back());
		status.lv_reg_offset.push_back(status.lv_reg_offset.back());
		status.var_maps.push_back(std::map<std::string, var_info*>());

		if (ast->d.for_d.init != nullptr) {
			codegen_preprocess_statement(ast->d.for_d.init, status);
		}
		if (ast->d.for_d.cond != nullptr) {
			codegen_preprocess_statement_expr(&ast->d.for_d.cond, ast->lineno, status);
		}
		if (ast->d.for_d.post != nullptr) {
			codegen_preprocess_statement_expr(&ast->d.for_d.post, ast->lineno, status);
		}
		codegen_preprocess_statement(ast->d.for_d.body, status);

		// 仮想的に作ったブロック用の情報を破棄する
		status.lv_mem_offset.pop_back();
		status.lv_reg_offset.pop_back();
		status.var_maps.pop_back();
		break;
	case NODE_GOTO:
	case NODE_CONTINUE:
	case NODE_BREAK:
		// 何もしない
		break;
	case NODE_RETURN:
		if (ast->d.ret.ret_expression != nullptr) {
			if (is_void_type(status.return_type)) {
				throw codegen_error(ast->lineno, "return with expression found in void function");
			}
			codegen_preprocess_statement_expr(&ast->d.ret.ret_expression, ast->lineno, status);
			if (!is_assignable(status.return_type, ast->d.ret.ret_expression)) {
				throw codegen_error(ast->lineno, "invalid return type");
			}
		} else {
			if (!is_void_type(status.return_type)) {
				throw codegen_error(ast->lineno, "return without expression found in non-void function");
			}
		}
		break;
	default:
		throw codegen_error(ast->lineno, "unexpected node");
	}
}
