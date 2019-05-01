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
