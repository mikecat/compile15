#include <string>
#include <sstream>
#include <map>
#include <vector>
#include "ast.h"
#include "codegen.hpp"

std::string codegen_error::build_message(int lineno, std::string message) {
	std::stringstream ss;
	ss << message << " at line " << lineno;
	return ss.str();
}

struct var_info {
	int offset;
	type_node* type;
	bool is_global;
	bool is_register;
	var_info(int offset_ = 0, type_node* type_ = nullptr, bool isg = false, bool isr = false) :
		offset(offset_), type(type_), is_global(isg), is_register(isr) {}
};

struct codegen_status {
	// global
	int gv_offset;
	bool gv_exists;
	// global + function-local
	std::vector<std::map<std::string, var_info*> > var_maps;

	// function-local
	int lv_mem_size;
	std::vector<int> lv_mem_offset;
	int lv_reg_size;
	std::vector<int> lv_reg_offset;
	std::vector<int> lv_reg_assign;

	bool call_exists;
	int registers_written;
	int registers_reserved;
};

std::vector<asm_inst> codegen_gvar(ast_node* ast, codegen_status& status) {
	if (ast == nullptr || ast->kind != NODE_VAR_DEFINE) {
		throw codegen_error(ast == nullptr ? 0 : ast->lineno,
			"non-variable node passed to codegen_gvar()");
	}
	type_node* type = ast->d.var_def.type;
	char* name = ast->d.var_def.name;
	expression_node* initializer = ast->d.var_def.initializer;
	auto& var_map = status.var_maps.back();
	if (var_map.find(name) != var_map.end()) {
		throw codegen_error(ast->lineno,
			std::string("multiple definition of global variable ") + name);
	}
	int align = type->align;
	if (status.gv_offset % align != 0) {
		status.gv_offset = ((status.gv_offset + align - 1) / align) * align;
	}
	var_map[name] = new var_info(status.gv_offset, type, true, false);
	status.gv_offset += type->size;
	if (type->size == 1) status.gv_offset++; // DATABで1個だけデータを置いても2バイト使われる
	std::vector<asm_inst> result;
	std::vector<uint32_t> init_values;
	asm_inst_kind inst = EMPTY;
	type_node* element_type;
	int nelem;
	if (type->kind == TYPE_ARRAY) {
		element_type = type->info.element_type;
		if (element_type->kind == TYPE_ARRAY) {
			throw codegen_error(ast->lineno, "array of array not supported");
		}
		nelem = type->size / element_type->size;
		std::vector<uint32_t> array_init_values;
		if (initializer != nullptr) {
			while (initializer->kind == EXPR_OPERATOR && initializer->info.op.kind == OP_COMMA) {
				expression_node* left = initializer->info.op.operands[0];
				expression_node* right = initializer->info.op.operands[1];
				if (right->kind == EXPR_INTEGER_LITERAL) {
					array_init_values.push_back(right->info.value);
				} else {
					throw codegen_error(ast->lineno, "non-constant initializer");
				}
				initializer = left;
			}
			if (initializer->kind == EXPR_INTEGER_LITERAL) {
				array_init_values.push_back(initializer->info.value);
			} else {
				throw codegen_error(ast->lineno, "non-constant initializer");
			}
			init_values.insert(init_values.end(), array_init_values.rbegin(), array_init_values.rend());
		}
	} else {
		element_type = type;
		nelem = 1;
		if (initializer != nullptr) {
			if (initializer->kind == EXPR_INTEGER_LITERAL) {
				init_values.push_back(initializer->info.value);
			} else {
				throw codegen_error(ast->lineno, "non-constant initializer");
			}
		}
	}
	uint32_t mask = 0;
	switch (element_type->size) {
	case 1: inst = DB; mask = UINT32_C(0xff); break;
	case 2: inst = DW; mask = UINT32_C(0xffff); break;
	case 4: inst = DD; mask = UINT32_C(0xffffffff); break;
	default: throw codegen_error(ast->lineno, "unsupported element size");
	}
	auto init_itr = init_values.begin();
	for (int i = 0; i < nelem; i++) {
		uint32_t value;
		if (init_itr == init_values.end()) {
			value = 0;
		} else {
			value = *init_itr;
			init_itr++;
		}
		result.push_back(asm_inst(inst, value & mask));
		if (i == 0) result.back().comment = name;
	}
	return result;
}

// 今のブロックに変数を登録する
void codegen_register_variable(ast_node* var_def_node, codegen_status& status, bool argument_mode = false) {
	if (var_def_node == nullptr || var_def_node->kind != NODE_VAR_DEFINE) {
		throw codegen_error(var_def_node == nullptr ? 0 : var_def_node->lineno,
			"tried to register something not a variable");
	}
	auto& mem_offset = status.lv_mem_offset.back();
	auto& reg_offset = status.lv_reg_offset.back();
	auto& var_map = status.var_maps.back();
	type_node* type = var_def_node->d.var_def.type;
	char* name = var_def_node->d.var_def.name;
	if (var_map.find(name) != var_map.end()) {
		throw codegen_error(var_def_node->lineno,
			std::string("multiple definition of ") + (argument_mode ? "argument " : "variable ") + name);
	}
	var_info* vi;
	if (var_def_node->d.var_def.is_register) {
		vi = new var_info(reg_offset, type, false, true);
		reg_offset++;
		if (status.lv_reg_size < reg_offset) {
			status.lv_reg_size = reg_offset;
			status.lv_reg_assign.resize(status.lv_reg_size, -1);
		}
	} else {
		if (mem_offset % type->align != 0) {
			mem_offset += type->align - (mem_offset % type->align);
		}
		vi = new var_info(mem_offset, type, false, false);
		mem_offset += argument_mode ? 4 : type->size;
		if (status.lv_mem_size < mem_offset) status.lv_mem_size = mem_offset;
	}
	var_map[name] = vi;
}

// 式中の識別子を解決する
void codegen_resolve_identifier_expr(expression_node* expr, int lineno, codegen_status& status) {
	if (expr == nullptr) {
		throw codegen_error(lineno, "NULL passed to codegen_resolve_identifier_expr()");
	}
	switch (expr->kind) {
	case EXPR_INTEGER_LITERAL:
		// 何もしない
		break;
	case EXPR_IDENTIFIER:
		// 識別子なので、該当するものを内側のブロックから順に探す
		for (auto itr = status.var_maps.rbegin(); itr != status.var_maps.rend(); itr++) {
			auto ident_info = itr->find(expr->info.ident.name);
			if (ident_info != itr->end()) {
				// 見つかったので、情報をセットして終了
				expr->info.ident.info = ident_info->second;
				expr->type = ident_info->second->type;
				return;
			}
		}
		// 見つからなかったのでエラー
		throw codegen_error(lineno, std::string("identifier ") + expr->info.ident.name + " not found");
	case EXPR_OPERATOR:
		codegen_resolve_identifier_expr(expr->info.op.operands[0], lineno, status);
		if (expr->info.op.kind > OP_DUMMY_BINARY_START) {
			codegen_resolve_identifier_expr(expr->info.op.operands[1], lineno, status);
		}
		if (expr->info.op.kind > OP_DUMMY_TERNARY_START) {
			codegen_resolve_identifier_expr(expr->info.op.operands[2], lineno, status);
		}
		// オペランドの型が決まったはずなので、その計算結果の型を決め直す
		set_operator_expression_type(expr);
		if (expr->type == NULL) {
			throw codegen_error(lineno, "operand type error");
		}
		break;
	}
}

// ブロック中の識別子を解決する
void codegen_resolve_identifier_block(ast_node* ast, codegen_status& status) {
	if (ast == nullptr || ast->kind != NODE_ARRAY) {
		throw codegen_error(ast == nullptr ? 0 : ast->lineno,
			"non-array node passed to codegen_resolve_identifier_block()");
	}
	// このブロック用の情報を作る
	status.lv_mem_offset.push_back(status.lv_mem_offset.back());
	status.lv_reg_offset.push_back(status.lv_reg_offset.back());
	status.var_maps.push_back(std::map<std::string, var_info*>());

	size_t num = ast->d.array.num;
	ast_node** nodes = ast->d.array.nodes;
	for (size_t i = 0; i < num; i++) {
		switch (nodes[i]->kind) {
		case NODE_ARRAY:
			codegen_resolve_identifier_block(nodes[i], status);
			break;
		case NODE_VAR_DEFINE:
			codegen_register_variable(nodes[i], status);
			break;
		case NODE_FUNC_DEFINE:
			throw codegen_error(ast->lineno, "cannot define function inside function");
			break;
		case NODE_EXPR:
			codegen_resolve_identifier_expr(nodes[i]->d.expr.expression, nodes[i]->lineno, status);
			break;
		case NODE_EMPTY:
			// 何もしない
			break;
		case NODE_PRAGMA:
			// 何もしない
			break;
		default:
			throw codegen_error(ast->lineno, "unexpected node");
		}
	}

	// このブロック用の情報を破棄する
	status.lv_mem_offset.pop_back();
	status.lv_reg_offset.pop_back();
	status.var_maps.pop_back();
}

// 指定のレジスタに指定の数を置くコードを生成する
std::vector<asm_inst> codegen_put_number(int dest_reg, uint32_t value) {
	std::vector<asm_inst> res[3];
	uint32_t work_values[3] = {value, -value, ~value};
	for (int i = 0; i < 3; i++) {
		uint32_t work_value = work_values[i];
		bool is_first = true;
		for (int j = 31; j >= 7; j--) {
			if ((work_value >> j) & 1) {
				uint32_t current_value = (work_value >> (j - 7)) & 0xff;
				res[i].push_back(asm_inst(is_first ? MOV_LIT : ADD_LIT, dest_reg, current_value));
				if (j > 7) res[i].push_back(asm_inst(SHL_REG_LIT, dest_reg, dest_reg, j - 7));
				work_value &= ~(UINT32_C(0xff) << (j - 7));
				is_first = false;
			}
		}
		if (is_first) {
			res[i].push_back(asm_inst(MOV_LIT, dest_reg, work_value));
		} else if (work_value > 0) {
			res[i].push_back(asm_inst(ADD_LIT, dest_reg, work_value));
		}
	}
	res[1].push_back(asm_inst(NEG_REG, dest_reg, dest_reg));
	res[2].push_back(asm_inst(NOT_REG, dest_reg, dest_reg));
	int best = 0;
	if (res[1].size() < res[best].size()) best = 1;
	if (res[2].size() < res[best].size()) best = 2;
	return res[best];
}

std::vector<asm_inst> codegen_func(ast_node* ast, codegen_status& status) {
	if (ast == nullptr || ast->kind != NODE_FUNC_DEFINE) {
		throw codegen_error(ast == nullptr ? 0 : ast->lineno,
			"non-function node passed to codegen_func()");
	}
	// funciton-localなstatusを初期化
	status.lv_mem_size = 0;
	status.lv_mem_offset.clear();
	status.lv_mem_offset.push_back(0);
	status.lv_reg_size = 0;
	status.lv_reg_offset.clear();
	status.lv_reg_offset.push_back(0);
	status.lv_reg_assign.clear();
	status.call_exists = true; // TODO: 後でfalseにしてちゃんと調べる
	status.registers_written = 0;
	status.registers_reserved = 0;
	// 引数の情報を登録
	status.var_maps.push_back(std::map<std::string, var_info*>());
	int args_on_stack = 0;
	size_t args_num = 0;
	if (ast->d.func_def.arguments != NULL && ast->d.func_def.arguments->kind == NODE_ARRAY) {
		args_num = ast->d.func_def.arguments->d.array.num;
		if (args_num > 4) {
			throw codegen_error(ast->lineno, "too many arguments");
		}
		ast_node** args = ast->d.func_def.arguments->d.array.nodes;
		for (size_t i = 0; i < args_num; i++) {
			codegen_register_variable(args[i], status, true);
			// codegen_register_variable()内でチェックしているので、args[i]はNODE_VER_DEFINE
			if (!args[i]->d.var_def.is_register) args_on_stack |= 1 << i;
		}
	}
	int args_mem_size = status.lv_mem_size;
	// 識別子の情報を解決する
	codegen_resolve_identifier_block(ast->d.func_def.body, status);

	// レジスタ変数にレジスタを割り当てる
	if (status.gv_exists) {
		// R7をグローバル変数領域へのポインタ用に予約する
		status.registers_reserved |= 1 << 7;
	}
	// 割り当てるレジスタが指定された変数用のレジスタを予約する
	for (auto itr = status.lv_reg_assign.begin(); itr != status.lv_reg_assign.end(); itr++) {
		if (*itr >= 0) {
			if (*itr > 7) {
				throw codegen_error(ast->lineno, "invalid register specified");
			}
			if ((status.registers_reserved >> *itr) & 1) {
				throw codegen_error(ast->lineno, "register collision");
			}
			status.registers_reserved |= 1 << *itr;
		}
	}
	// 残りの変数を空いているレジスタに割り当てる
	static const int regs_try_order_candidate[2][8] = {
		{3, 2, 1, 0, 7, 6, 5, 4}, // 関数呼び出しが無い時
		{7, 6, 5, 4, 3, 2, 1, 0} // 関数呼び出しがある時
	};
	const int* regs_try_order = regs_try_order_candidate[status.call_exists ? 1 : 0];
	for (int i = 0; i < status.lv_reg_size; i++) {
		if (status.lv_reg_assign[i] < 0) {
			bool ok = false;
			for (int j = 0; j < 8; j++) {
				int reg = regs_try_order[j];
				if (!((status.registers_reserved >> reg) & 1)) {
					status.lv_reg_assign[i] = reg;
					status.registers_reserved |= 1 << reg;
					ok = true;
					break;
				}
			}
			if (!ok) throw codegen_error(ast->lineno, "register exhausted for variables");
		}
	}

	std::vector<asm_inst> result;
	// スタックにローカル変数の領域を確保する
	if (status.lv_mem_size > args_mem_size) {
		int delta = (status.lv_mem_size - args_mem_size + 3) / 4;
		if (delta <= 127) {
			result.push_back(asm_inst(SUBSP_LIT, delta));
		} else {
			int reg = args_num < 4 ? 3 : 4;
			status.registers_written |= 1 << reg;
			std::vector<asm_inst> num_code = codegen_put_number(reg, -delta * 4);
			result.insert(result.end(), num_code.begin(), num_code.end());
			result.push_back(asm_inst(ADD_REG, 13, reg));
		}
	}
	// スタックに引数を積む
	if (args_on_stack != 0) {
		result.push_back(asm_inst(PUSH_REGS, args_on_stack));
	}

	// 本体のコードを生成する

	// 値を書き込んだcallee-saveレジスタの退避コードを追加する
	int regs_to_backup = status.registers_written & 0xf0;
	if (status.call_exists) regs_to_backup |= 0x100;
	if (regs_to_backup != 0) {
		result.insert(result.begin(), asm_inst(PUSH_REGS, regs_to_backup));
	}
	// 関数のラベルを追加する
	result.insert(result.begin(), asm_inst(LABEL, ast->d.func_def.name));

	// スタック上の引数とローカル変数を取り除く
	if (status.lv_mem_size > 0) {
		int delta = (status.lv_mem_size + 3) / 4;
		if (delta <= 127) {
			result.push_back(asm_inst(ADDSP_LIT, delta));
		} else {
			std::vector<asm_inst> num_code = codegen_put_number(3, delta * 4);
			result.insert(result.end(), num_code.begin(), num_code.end());
			result.push_back(asm_inst(ADD_REG, 13, 3));
		}
	}
	// 退避したレジスタを戻して関数から戻る
	if (regs_to_backup != 0) {
		result.push_back(asm_inst(POP_REGS, regs_to_backup));
	}
	if (!(regs_to_backup & 0x100)) {
		result.push_back(asm_inst(RET));
	}

	// 引数の情報を破棄
	status.var_maps.pop_back();

	return result;
}

std::vector<asm_inst> codegen(ast_node* ast) {
	if (ast == nullptr || ast->kind != NODE_ARRAY) {
		throw codegen_error(ast == nullptr ? 0 : ast->lineno,
			"top-level AST not array");
	}
	std::vector<asm_inst> result;
	codegen_status status;
	status.gv_offset = 0;
	status.gv_exists = false;
	status.var_maps.push_back(std::map<std::string, var_info*>());

	for (size_t i = 0; i < ast->d.array.num; i++) {
		ast_node* node = ast->d.array.nodes[i];
		if (node->kind == NODE_VAR_DEFINE) {
			std::vector<asm_inst> var_inst = codegen_gvar(node, status);
			result.insert(result.end(), var_inst.begin(), var_inst.end());
			status.gv_exists = true;
		} else if (node->kind == NODE_FUNC_DEFINE) {
			status.var_maps.back()[node->d.func_def.name] = new var_info(0,
				new_function_type(node->d.func_def.return_type, node->d.func_def.arguments), true, false);
		}
	}

	for (size_t i = 0; i < ast->d.array.num; i++) {
		if (ast->d.array.nodes[i]->kind == NODE_FUNC_DEFINE) {
			std::vector<asm_inst> func_inst = codegen_func(ast->d.array.nodes[i], status);
			result.insert(result.end(), func_inst.begin(), func_inst.end());
		}
	}

	return result;
}
