#include <string>
#include <sstream>
#include <map>
#include <vector>
#include "ast.h"
#include "codegen.hpp"
#include "codegen_internal.hpp"

std::string codegen_error::build_message(int lineno, std::string message) {
	std::stringstream ss;
	ss << message << " at line " << lineno;
	return ss.str();
}

// グローバル変数のコードを生成する
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
				expression_node* value_node = constfold(right);
				if (value_node->kind == EXPR_INTEGER_LITERAL) {
					array_init_values.push_back(value_node->info.value);
				} else {
					throw codegen_error(ast->lineno, "non-constant initializer");
				}
				initializer = left;
			}
			expression_node* initializer_value_node = constfold(initializer);
			if (initializer_value_node->kind == EXPR_INTEGER_LITERAL) {
				array_init_values.push_back(initializer_value_node->info.value);
			} else {
				throw codegen_error(ast->lineno, "non-constant initializer");
			}
			init_values.insert(init_values.end(), array_init_values.rbegin(), array_init_values.rend());
		}
	} else {
		element_type = type;
		nelem = 1;
		if (initializer != nullptr) {
			expression_node* initializer_value_node = constfold(initializer);
			if (initializer_value_node->kind == EXPR_INTEGER_LITERAL) {
				init_values.push_back(initializer_value_node->info.value);
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

// グローバル変数アクセス用のレジスタを設定する
std::vector<asm_inst> codegen_set_gv_access_register(int dest_reg, int src_reg,
int base_address, codegen_status& status) {
	std::vector<asm_inst> result;
	if (status.old_entry) {
		// グローバル変数の位置が渡されるので、格納する
		if (src_reg != dest_reg) {
			result.push_back(asm_inst(MOV_REG, dest_reg, src_reg));
		}
	} else {
		// RAM領域の0番地の位置が渡されるので、base_addressを足して格納する
		std::vector<asm_inst> num_code = codegen_put_number(dest_reg, base_address);
		if (src_reg == dest_reg) result.push_back(asm_inst(MOV_REG, 12, src_reg));
		result.insert(result.end(), num_code.begin(), num_code.end());
		result.push_back(asm_inst(ADD_REG, dest_reg, src_reg == dest_reg ? 12 : src_reg));
	}
	return result;
}

// 関数定義のコードを生成する
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
	status.call_exists = false;
	status.gv_access_exists = false;
	status.gv_access_register = -1;
	status.registers_written = 0;
	status.registers_reserved = 0;
	// 引数の情報を登録
	status.var_maps.push_back(std::map<std::string, var_info*>());
	int args_on_stack = 0, args_on_reg = 0;
	size_t args_num = 0;
	std::vector<int> reg_args_given;
	std::vector<int> reg_args_assigned;
	bool r1_is_reg_arg = false; // R1はレジスタに保存する引数として使用されている
	if (ast->d.func_def.arguments != NULL && ast->d.func_def.arguments->kind == NODE_ARRAY) {
		args_num = ast->d.func_def.arguments->d.array.num;
		if (args_num > 4) {
			throw codegen_error(ast->lineno, "too many arguments");
		}
		ast_node** args = ast->d.func_def.arguments->d.array.nodes;
		for (size_t i = 0; i < args_num; i++) {
			int assigned = codegen_register_variable(args[i], status, true, false, 0);
			// codegen_register_variable()内でチェックしているので、args[i]はNODE_ARGUMENT
			if (args[i]->d.arg.is_register) {
				args_on_reg |= 1 << i;
				reg_args_given.push_back(i);
				reg_args_assigned.push_back(assigned);
			} else {
				args_on_stack |= 1 << i;
			}
		}
		r1_is_reg_arg = args_num >= 2 && args[1]->d.arg.is_register;
	}
	int args_mem_size = status.lv_mem_size;
	// コード生成に備えた前処理を行う
	codegen_preprocess_block(ast->d.func_def.body, status);

	// レジスタ変数にレジスタを割り当てる
	// グローバル変数が無ければ、アクセス用のレジスタは不要
	// entry関数かつ関数呼び出しが無ければ、好きなレジスタを使えばいいので予約不要
	// 関数呼び出しが無く、かつグローバル変数へのアクセスが無ければ、アクセス用のレジスタは不要
	if (status.gv_exists && ((!status.entry_function && status.gv_access_exists) || status.call_exists)) {
		// R7をグローバル変数領域へのポインタ用に予約する
		status.registers_reserved |= 1 << 7;
		status.gv_access_register = 7;
	}
	// 割り当てるレジスタが指定された変数用のレジスタを予約する
	for (auto itr = status.lv_reg_assign.begin(); itr != status.lv_reg_assign.end(); itr++) {
		if (*itr >= 0) {
			if (*itr > 7) {
				throw codegen_error(ast->lineno, "invalid register specified");
			}
			if ((status.registers_reserved >> *itr) & 1) {
				throw codegen_error(ast->lineno, "register specification conflict");
			}
			status.registers_reserved |= 1 << *itr;
		}
	}
	// 関数呼び出しが無いモードの時、レジスタの引数をそのレジスタに優先して割り当てる
	if (!status.call_exists) {
		for (size_t i = 0; i < reg_args_given.size(); i++) {
			if (status.lv_reg_assign[reg_args_assigned[i]] < 0 && // 割り当てが指定されていない
			!((status.registers_reserved >> reg_args_given[i]) & 1)) { // そのレジスタが空いている
				// 割り当てる
				status.lv_reg_assign[reg_args_assigned[i]] = reg_args_given[i];
				status.registers_reserved |= 1 << reg_args_given[i];
			}
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
	// グローバル変数へのアクセスがあり、かつまだアクセス用のレジスタを割り当てていなければ、割り当てる
	if (status.gv_access_exists && status.gv_access_register < 0) {
		bool ok = false;
		for (int j = 0; j < 8; j++) {
			int reg = regs_try_order[j];
			if (!((status.registers_reserved >> reg) & 1)) {
				status.gv_access_register = reg;
				status.registers_reserved |= 1 << reg;
				ok = true;
				break;
			}
		}
		if (!ok) throw codegen_error(ast->lineno, "register exhausted for global variable access");
	}

	std::vector<asm_inst> result;
	// entry関数かつグローバル変数の位置を用いる場合、グローバル変数の位置を設定する
	bool write_gv_access_register = status.entry_function && status.gv_access_register >= 0;
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
	// レジスタ上の変数を割り当てたレジスタに書き込む
	for (size_t i = 0; i < reg_args_assigned.size(); i++) {
		// オフセットからそのオフセットに対応するレジスタに変換する
		reg_args_assigned[i] = status.lv_reg_assign[reg_args_assigned[i]];
	}
	bool scheduled_r1_is_gv_access_register = false;
	if (write_gv_access_register && !r1_is_reg_arg) {
		// グローバル変数アクセス用のレジスタの設定が要求され、
		// かつR1をメモリに置くか保存しない場合は、スケジューリングに組み込む
		scheduled_r1_is_gv_access_register = true;
		reg_args_given.push_back(1);
		reg_args_assigned.push_back(status.gv_access_register);
	}
	// 書き込む予定の所が読み込み対象なら、それを先に処理する
	// この関係がループしてしまったら、R12を用いて処理する
	int args_assign_done = 0;
	for (size_t i = 0; i < reg_args_given.size(); i++) {
		// 既に処理済みなら、何もしない
		if ((args_assign_done >> i) & 1) continue;
		// 割り当てられたレジスタとデータがあるレジスタが同じなら、移動しない
		if (reg_args_assigned[i] == reg_args_given[i]) {
			if (reg_args_given[i] == 1 && scheduled_r1_is_gv_access_register) {
				std::vector<asm_inst> gv_code = codegen_set_gv_access_register(
					reg_args_assigned[i], reg_args_given[i], status.base_address, status);
				result.insert(result.end(), gv_code.begin(), gv_code.end());
				status.registers_written |= 1 << reg_args_assigned[i];
			}
			continue;
		}

		std::vector<int> target;
		target.push_back(i);
		size_t current = i;
		bool hit;
		bool loop = false;
		do {
			hit = false;
			for (size_t j = 0; j < reg_args_given.size(); j++) {
				if (!((args_assign_done >> j) & 1) && reg_args_given[j] == reg_args_assigned[current]) {
					// まだ書き込んでいなくて、ぶつかるなら
					hit = true;
					target.push_back(j);
					current = j;
					break; // 同じレジスタが複数の引数にはなっていないはず
				}
			}
			if (hit && current == i) {
				loop = true;
				target.pop_back();
				break;
			}
		} while (hit);
		if (loop) {
			result.push_back(asm_inst(MOV_REG, 12, reg_args_given[target.back()]));
			auto itr = target.rbegin();
			for (itr++; itr != target.rend(); itr++) {
				if (scheduled_r1_is_gv_access_register && reg_args_given[*itr] == 1) {
					std::vector<asm_inst> gv_code = codegen_set_gv_access_register(
						reg_args_assigned[*itr], reg_args_given[*itr], status.base_address, status);
					result.insert(result.end(), gv_code.begin(), gv_code.end());
				} else {
					result.push_back(asm_inst(MOV_REG, reg_args_assigned[*itr], reg_args_given[*itr]));
				}
				status.registers_written |= 1 << reg_args_assigned[*itr];
				args_assign_done |= 1 << *itr;
			}
			if (scheduled_r1_is_gv_access_register && reg_args_given[target.back()] == 1) {
				std::vector<asm_inst> gv_code = codegen_set_gv_access_register(
					reg_args_assigned[target.back()], 12, status.base_address, status);
				result.insert(result.end(), gv_code.begin(), gv_code.end());
			} else {
				result.push_back(asm_inst(MOV_REG, reg_args_assigned[target.back()], 12));
			}
			status.registers_written |= 1 << reg_args_assigned[target.back()];
			args_assign_done |= 1 << target.back();
		} else {
			for (auto itr = target.rbegin(); itr != target.rend(); itr++) {
				if (scheduled_r1_is_gv_access_register && reg_args_given[*itr] == 1) {
					std::vector<asm_inst> gv_code = codegen_set_gv_access_register(
						reg_args_assigned[*itr], reg_args_given[*itr], status.base_address, status);
					result.insert(result.end(), gv_code.begin(), gv_code.end());
				} else {
					result.push_back(asm_inst(MOV_REG, reg_args_assigned[*itr], reg_args_given[*itr]));
				}
				status.registers_written |= 1 << reg_args_assigned[*itr];
				args_assign_done |= 1 << *itr;
			}
		}
	}
	if (write_gv_access_register && r1_is_reg_arg) {
		// グローバル変数アクセス用のレジスタの設定が要求され、
		// かつR1をレジスタに置く場合は、その配置後に処理する
		for (size_t i = 0; i < reg_args_given.size(); i++) {
			if (reg_args_given[i] == 1) {
				// R1の割り当て先からデータを取る
				std::vector<asm_inst> gv_code = codegen_set_gv_access_register(
					status.gv_access_register, reg_args_assigned[i], status.base_address, status);
				result.insert(result.end(), gv_code.begin(), gv_code.end());
				args_assign_done |= 1 << status.gv_access_register;
			}
		}
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

// 全体のコードを生成する
std::vector<asm_inst> codegen(ast_node* ast) {
	if (ast == nullptr || ast->kind != NODE_ARRAY) {
		throw codegen_error(ast == nullptr ? 0 : ast->lineno,
			"top-level AST not array");
	}
	std::vector<asm_inst> result;
	codegen_status status;
	status.base_address = 0x700;
	status.old_entry_exists = false;
	status.old_single_entry_exists = false;
	status.old_single_entry_name = "";
	status.entry_function = false;
	status.old_entry = false;
	status.old_single_entry = false;
	status.gv_offset = 0;
	status.gv_exists = false;
	status.var_maps.push_back(std::map<std::string, var_info*>());

	// グローバル変数を配置するコードを生成する
	// ついでにbase_addressの指定を拾う
	bool base_address_specified = false;
	for (size_t i = 0; i < ast->d.array.num; i++) {
		ast_node* node = ast->d.array.nodes[i];
		if (node->kind == NODE_VAR_DEFINE) {
			std::vector<asm_inst> var_inst = codegen_gvar(node, status);
			result.insert(result.end(), var_inst.begin(), var_inst.end());
			status.gv_exists = true;
		} else if (node->kind == NODE_FUNC_DEFINE) {
			status.var_maps.back()[node->d.func_def.name] = new var_info(0,
				new_function_type(node->d.func_def.return_type, node->d.func_def.arguments), true, false);
		} else if (node->kind == NODE_PRAGMA) {
			size_t token_num = node->d.array.num;
			ast_node** tokens = node->d.array.nodes;
			if (token_num >= 1 && tokens[0]->kind == NODE_CONTROL_IDENTIFIER &&
			std::string(tokens[0]->d.identifier.name) == "base_address") {
				if (base_address_specified) {
					throw codegen_error(node->lineno, "multiple base address specification found");
				}
				if (token_num >= 2 && tokens[1]->kind == NODE_CONTROL_INTEGER) {
					status.base_address = tokens[1]->d.integer.value;
					base_address_specified = true;
				} else {
					throw codegen_error(node->lineno, "invalid base address sepcification");
				}
			}
		}
	}
	// 最後にDATABが来たとき用に、アラインメントしておく
	if (status.gv_offset % 2 != 0) status.gv_offset++;

	// 余計なバイトが入らないように、連続するDATABをまとめる
	auto prev_itr = result.begin();
	asm_inst prev_inst = asm_inst(EMPTY);
	for (auto itr = result.begin(); itr != result.end(); itr++) {
		if (prev_inst.kind == DB && itr->kind == DB) {
			// DBが続いたので、まとめる
			asm_inst merged = asm_inst(DB2, prev_inst.params[0], itr->params[0]);
			std::string merged_comment;
			if (prev_inst.comment == "") {
				if (itr->comment == "") merged_comment = "";
				else merged_comment = std::string("*, ") + itr->comment;
			} else {
				if (itr->comment == "") merged_comment = prev_inst.comment;
				else merged_comment = prev_inst.comment + ", " + itr->comment;
			}
			merged.comment = merged_comment;
			itr = result.erase(prev_itr);
			itr = result.erase(itr);
			itr = result.insert(itr, merged);
			prev_itr = itr;
			prev_inst = asm_inst(EMPTY);
		} else {
			prev_itr = itr;
			prev_inst = *itr;
		}
	}

	// 関数のコードを生成する
	for (size_t i = 0; i < ast->d.array.num; i++) {
		if (ast->d.array.nodes[i]->kind == NODE_PRAGMA) {
			size_t token_num = ast->d.array.nodes[i]->d.array.num;
			ast_node** tokens = ast->d.array.nodes[i]->d.array.nodes;
			if (token_num >= 1 && tokens[0]->kind == NODE_CONTROL_IDENTIFIER &&
			std::string(tokens[0]->d.identifier.name) == "entry") {
				if (status.entry_function) {
					throw codegen_error(tokens[0]->lineno, "multiple entry specified for one function");
				}
				status.entry_function = true;
				for (size_t i = 1; i < token_num; i++) {
					if (tokens[i]->kind == NODE_CONTROL_IDENTIFIER) {
						std::string name = tokens[i]->d.identifier.name;
						if (name == "old") status.old_entry = true;
						else if (name == "single") status.old_single_entry = true;
					}
				}
			}
		} else {
			if (ast->d.array.nodes[i]->kind == NODE_FUNC_DEFINE) {
				if (status.entry_function && status.old_entry) {
					if (status.old_single_entry_exists ||
					(status.old_single_entry && status.old_entry_exists)) {
						throw codegen_error(ast->d.array.nodes[i]->lineno,
							"multiple old entry function found while single is specified");
					}
					status.old_entry_exists = true;
					if (status.old_single_entry) {
						status.old_single_entry_exists = true;
						status.old_single_entry_name = ast->d.array.nodes[i]->d.func_def.name;
					}
				}
				std::vector<asm_inst> func_inst = codegen_func(ast->d.array.nodes[i], status);
				result.insert(result.end(), func_inst.begin(), func_inst.end());
			}
			status.entry_function = false;
			status.old_entry = false;
			status.old_single_entry = false;
		}
	}

	// old entry用のコードを追加する
	if (status.old_entry_exists) {
		if (status.old_single_entry_exists) {
			result.insert(result.begin(), asm_inst(JMP_DIRECT, status.old_single_entry_name));
		} else {
			result.insert(result.begin(), asm_inst(ADD_REG, 15, 0));
		}
		result.insert(result.begin(), asm_inst(MOV_REG, 1, 15));
	}

	return result;
}
