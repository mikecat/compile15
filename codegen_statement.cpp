#include <vector>
#include "codegen_internal.hpp"

// 文のコード生成を行う
std::vector<asm_inst> codegen_statement(ast_node* ast, codegen_status& status) {
	if (ast == nullptr) {
		throw codegen_error(0, "NULL passed to codegen_statement()");
	}
	std::vector<asm_inst> result;
	switch (ast->kind) {
	case NODE_ARRAY: // ブロック
		{
			size_t num = ast->d.array.num;
			ast_node** nodes = ast->d.array.nodes;
			for (size_t i = 0; i < num; i++) {
				std::vector<asm_inst> sub_result = codegen_statement(nodes[i], status);
				result.insert(result.end(), sub_result.begin(), sub_result.end());
			}
		}
		break;
	case NODE_VAR_DEFINE:
		if (ast->d.var_def.initializer != nullptr) {
			throw codegen_error(ast->lineno, "variable initialization not supported yet");
		}
		break;
	case NODE_EXPR:
		{
			codegen_expr_result eres = codegen_expr(ast->d.expr.expression, ast->lineno, false, false,
				-1, 0xff & ~status.registers_reserved, 0, status);
			result.insert(result.end(), eres.insts.begin(), eres.insts.end());
		}
		break;
	case NODE_EMPTY:
		// 何もしない
		break;
	case NODE_PRAGMA:
		// 何もしない
		break;
	case NODE_LABEL:
		{
			if (status.goto_labels.find(ast->d.label.name) == status.goto_labels.end()) {
				throw codegen_error(ast->lineno, std::string("unknown label") + ast->d.label.name);
			}
			result.push_back(asm_inst(LABEL, get_label(status.goto_labels[ast->d.label.name])));
			std::vector<asm_inst> sub_result = codegen_statement(ast->d.label.statement, status);
			result.insert(result.end(), sub_result.begin(), sub_result.end());
		}
		break;
	case NODE_IF:
		{
			std::string skip_true_label = get_label(status.next_label++);
			std::string skip_false_label;
			// 条件式のコード生成
			std::vector<asm_inst> cond_result = codegen_conditional_jump(ast->d.if_d.cond, ast->lineno,
				skip_true_label, false, 0xff & ~status.registers_reserved, 0, status);
			result.insert(result.end(), cond_result.begin(), cond_result.end());
			// 条件式が真のとき実行する文のコード生成
			std::vector<asm_inst> true_result = codegen_statement(ast->d.if_d.true_statement, status);
			result.insert(result.end(), true_result.begin(), true_result.end());
			if (ast->d.if_d.false_statement != nullptr) {
				skip_false_label = get_label(status.next_label++);
				result.push_back(asm_inst(JMP_DIRECT, skip_false_label));
			}
			result.push_back(asm_inst(LABEL, skip_true_label));
			// 条件式が偽のとき実行する文のコード生成
			if (ast->d.if_d.false_statement != nullptr) {
				std::vector<asm_inst> false_result = codegen_statement(ast->d.if_d.false_statement, status);
				result.insert(result.end(), false_result.begin(), false_result.end());
				result.push_back(asm_inst(LABEL, skip_false_label));
			}
		}
		break;
	case NODE_SWITCH:
		{
			int available_regs = 0xff & ~status.registers_reserved;
			codegen_expr_result expr_result = codegen_expr(ast->d.switch_d.expr, ast->lineno, true, false,
				-1, available_regs, 0, status);
			available_regs &= ~(1 << expr_result.result_reg);
			int default_label = ast->d.switch_d.info->default_label;
			int end_label = status.next_label++;
			if (default_label < 0) {
				default_label = end_label;
			}
			// 分岐部分を生成する
			uint32_t prev_value = 0;
			int compare_reg = -1;
			auto& case_labels = ast->d.switch_d.info->case_labels;
			for (auto itr = case_labels.begin(); itr != case_labels.end(); itr++) {
				uint32_t value = itr->first;
				std::string label = get_label(itr->second);
				if (value < 256) {
					// u8と比較する命令で直接比較する
					result.push_back(asm_inst(CMP_REG_LIT, expr_result.result_reg, value));
					result.push_back(asm_inst(JCC, EQ, label));
				} else {
					// 比較対象の値をレジスタに入れて比較する
					if (compare_reg < 0) {
						// レジスタを割り当てる
						compare_reg = get_reg_to_use(ast->lineno, available_regs, false);
						std::vector<asm_inst> ncode = codegen_put_number(compare_reg, value);
						result.insert(result.end(), ncode.begin(), ncode.end());
					} else if (value - prev_value < 256) {
						// 前の値との差分が小さいので、差分を足す
						result.push_back(asm_inst(ADD_LIT, compare_reg, value - prev_value));
					} else {
						// 値を最初から生成する
						std::vector<asm_inst> ncode = codegen_put_number(compare_reg, value);
						result.insert(result.end(), ncode.begin(), ncode.end());
					}
					prev_value = value;
					result.push_back(asm_inst(CMP_REG_REG, expr_result.result_reg, compare_reg));
					result.push_back(asm_inst(JCC, EQ, label));
				}
			}
			result.push_back(asm_inst(JMP_DIRECT, get_label(default_label)));
			// 中身の文のコードを生成する
			status.break_labels.push_back(end_label);
			std::vector<asm_inst> sub_result = codegen_statement(ast->d.switch_d.statement, status);
			result.insert(result.end(), sub_result.begin(), sub_result.end());
			result.push_back(asm_inst(LABEL, get_label(end_label)));
			status.break_labels.pop_back();
		}
		break;
	case NODE_CASE:
		{
			result.push_back(asm_inst(LABEL, get_label(ast->d.case_d.info->label_id)));
			std::vector<asm_inst> sub_result = codegen_statement(ast->d.case_d.statement, status);
			result.insert(result.end(), sub_result.begin(), sub_result.end());
		}
		break;
	case NODE_DEFAULT:
		{
			result.push_back(asm_inst(LABEL, get_label(ast->d.default_d.info->label_id)));
			std::vector<asm_inst> sub_result = codegen_statement(ast->d.default_d.statement, status);
			result.insert(result.end(), sub_result.begin(), sub_result.end());
		}
		break;
	case NODE_WHILE:
	case NODE_DO_WHILE:
		{
			int continue_label_id = status.next_label++;
			int loop_label_id = status.next_label++;
			int break_label_id = status.next_label++;
			std::string continue_label = get_label(continue_label_id);
			std::string loop_label = get_label(loop_label_id);
			std::string break_label = get_label(break_label_id);
			// continueとbreakに使うラベル情報を登録する
			status.continue_labels.push_back(continue_label_id);
			status.break_labels.push_back(break_label_id);
			// 条件式の評価からループを開始させる
			if (ast->kind == NODE_WHILE) {
				result.push_back(asm_inst(JMP_DIRECT, continue_label));
			}
			// ループ本体
			result.push_back(asm_inst(LABEL, loop_label));
			std::vector<asm_inst> body_result = codegen_statement(ast->d.while_d.statement, status);
			result.insert(result.end(), body_result.begin(), body_result.end());
			// 条件式
			result.push_back(asm_inst(LABEL, continue_label));
			std::vector<asm_inst> cond_result = codegen_conditional_jump(ast->d.while_d.cond, ast->lineno,
				loop_label, true, 0xff & ~status.registers_reserved, 0, status);
			result.insert(result.end(), cond_result.begin(), cond_result.end());
			// ループ終了
			result.push_back(asm_inst(LABEL, break_label));
			// continueとbreakに使うラベル情報を破棄する
			status.continue_labels.pop_back();
			status.break_labels.pop_back();
		}
		break;
	case NODE_FOR:
		{
			int initial_label_id = status.next_label++;
			int continue_label_id = status.next_label++;
			int loop_label_id = status.next_label++;
			int break_label_id = status.next_label++;
			std::string initial_label = get_label(initial_label_id);
			std::string continue_label = get_label(continue_label_id);
			std::string loop_label = get_label(loop_label_id);
			std::string break_label = get_label(break_label_id);
			// continueとbreakに使うラベル情報を登録する
			status.continue_labels.push_back(continue_label_id);
			status.break_labels.push_back(break_label_id);
			// 初期化
			std::vector<asm_inst> init_result = codegen_statement(ast->d.for_d.init, status);
			result.insert(result.end(), init_result.begin(), init_result.end());
			// 条件式の評価からループを開始させる
			result.push_back(asm_inst(JMP_DIRECT, initial_label));
			// ループ本体
			result.push_back(asm_inst(LABEL, loop_label));
			std::vector<asm_inst> body_result = codegen_statement(ast->d.for_d.body, status);
			result.insert(result.end(), body_result.begin(), body_result.end());
			// 更新式
			result.push_back(asm_inst(LABEL, continue_label));
			if (ast->d.for_d.post != nullptr) {
				codegen_expr_result post_result = codegen_expr(ast->d.for_d.post, ast->lineno,
					false, false, -1, 0xff & ~status.registers_reserved, 0, status);
				result.insert(result.end(), post_result.insts.begin(), post_result.insts.end());
			}
			// 条件式
			result.push_back(asm_inst(LABEL, initial_label));
			std::vector<asm_inst> cond_result;
			if (ast->d.for_d.cond != nullptr) {
				cond_result = codegen_conditional_jump(ast->d.for_d.cond, ast->lineno,
					loop_label, true, 0xff & ~status.registers_reserved, 0, status);
			} else {
				cond_result.push_back(asm_inst(JMP_DIRECT, loop_label));
			}
			result.insert(result.end(), cond_result.begin(), cond_result.end());
			// ループ終了
			result.push_back(asm_inst(LABEL, break_label));
			// continueとbreakに使うラベル情報を破棄する
			status.continue_labels.pop_back();
			status.break_labels.pop_back();
		}
		break;
	case NODE_GOTO:
		if (status.goto_labels.find(ast->d.label.name) == status.goto_labels.end()) {
			throw codegen_error(ast->lineno, std::string("unknown label") + ast->d.label.name);
		}
		result.push_back(asm_inst(JMP_DIRECT, get_label(status.goto_labels[ast->d.label.name])));
		break;
	case NODE_CONTINUE:
		if (status.continue_labels.empty()) {
			throw codegen_error(ast->lineno, "continue without anything to continue");
		}
		result.push_back(asm_inst(JMP_DIRECT, get_label(status.continue_labels.back())));
		break;
	case NODE_BREAK:
		if (status.break_labels.empty()) {
			throw codegen_error(ast->lineno, "break without anything to break");
		}
		result.push_back(asm_inst(JMP_DIRECT, get_label(status.break_labels.back())));
		break;
	case NODE_RETURN:
		if (ast->d.ret.ret_expression != nullptr) {
			codegen_expr_result eres = codegen_expr(ast->d.ret.ret_expression, ast->lineno, true, false,
				0, 0xff & ~status.registers_reserved, 0, status);
			result.insert(result.end(), eres.insts.begin(), eres.insts.end());
			if (eres.result_reg != 0) {
				result.push_back(asm_inst(MOV_REG, 0, eres.result_reg));
			}
		}
		result.push_back(asm_inst(JMP_DIRECT, get_label(status.return_label)));
		break;
	default:
		throw codegen_error(ast->lineno, "unexpected node passed to codegen_statement()");
	}
	return result;
}
