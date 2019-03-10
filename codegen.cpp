#include <string>
#include <sstream>
#include <map>
#include "codegen.hpp"

std::string codegen_error::build_message(int lineno, std::string message) {
	std::stringstream ss;
	ss << message << " at line " << lineno;
	return ss.str();
}

struct var_info {
	int addr;
	type_node* type;
	var_info(int addr_ = 0, type_node* type_ = nullptr) : addr(addr_), type(type_) {}
};

struct codegen_status {
	int gv_addr;
	std::map<std::string, var_info> gv_map;
};

std::vector<asm_inst> codegen_gvar(ast_node* ast, codegen_status& status) {
	if (ast == nullptr || ast->kind != NODE_VAR_DEFINE) {
		throw codegen_error(ast == nullptr ? 0 : ast->lineno,
			"non-variable node passed to codegen_gvar()");
	}
	type_node* type = ast->d.var_def.type;
	char* name = ast->d.var_def.name;
	expression_node* initializer = ast->d.var_def.initializer;
	if (status.gv_map.find(name) != status.gv_map.end()) {
		throw codegen_error(ast->lineno,
			std::string("multiple definition of global variable ") + name);
	}
	int align = type->align;
	if (status.gv_addr % align != 0) {
		status.gv_addr = ((status.gv_addr + align - 1) / align) * align;
	}
	status.gv_map[name] = var_info(status.gv_addr, type);
	status.gv_addr += type->size;
	if (type->size == 1) status.gv_addr++; // DATABで1個だけデータを置いても2バイト使われる
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

std::vector<asm_inst> codegen_func(ast_node* ast, codegen_status& status) {
	if (ast == nullptr || ast->kind != NODE_FUNC_DEFINE) {
		throw codegen_error(ast == nullptr ? 0 : ast->lineno,
			"non-function node passed to codegen_func()");
	}
	std::vector<asm_inst> result;
	result.push_back(asm_inst(LABEL, ast->d.func_def.name));
	result.push_back(asm_inst(RET));
	return result;
}

std::vector<asm_inst> codegen(ast_node* ast) {
	if (ast == nullptr || ast->kind != NODE_ARRAY) {
		throw codegen_error(ast == nullptr ? 0 : ast->lineno,
			"top-level AST not array");
	}
	std::vector<asm_inst> result;
	codegen_status status;
	status.gv_addr = 0;
	status.gv_map.clear();

	for (size_t i = 0; i < ast->d.array.num; i++) {
		if (ast->d.array.nodes[i]->kind == NODE_VAR_DEFINE) {
			std::vector<asm_inst> var_inst = codegen_gvar(ast->d.array.nodes[i], status);
			result.insert(result.end(), var_inst.begin(), var_inst.end());
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
