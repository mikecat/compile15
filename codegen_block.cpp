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
			codegen_expr_result eres = codegen_expr(ast->d.expr.expression, ast->lineno, false,
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
	case NODE_RETURN:
		if (ast->d.ret.ret_expression != nullptr) {
			codegen_expr_result eres = codegen_expr(ast->d.ret.ret_expression, ast->lineno, true,
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
