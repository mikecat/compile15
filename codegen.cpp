#include <stdexcept>
#include "codegen.hpp"

std::vector<asm_inst> codegen_func(ast_node* ast) {
	if (ast == nullptr || ast->kind != NODE_FUNC_DEF) {
		throw std::runtime_error("non-function node passed to codegen_func()");
	}
	std::vector<asm_inst> result;
	result.push_back(asm_inst(LABEL, ast->d.func_def.name));
	result.push_back(asm_inst(RET));
	return result;
}

std::vector<asm_inst> codegen(ast_node* ast) {
	if (ast == nullptr || ast->kind != NODE_ARRAY) {
		throw std::runtime_error("top-level AST not array");
	}
	std::vector<asm_inst> result;
	for (size_t i = 0; i < ast->d.array.num; i++) {
		std::vector<asm_inst> func_inst = codegen_func(ast->d.array.nodes[i]);
		result.insert(result.end(), func_inst.begin(), func_inst.end());
	}
	return result;
}
