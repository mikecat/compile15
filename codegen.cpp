#include <string>
#include <map>
#include <stdexcept>
#include "codegen.hpp"

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
	if (ast == nullptr || ast->kind != NODE_VAR_DEF) {
		throw std::runtime_error("non-variable node passed to codegen_gvar()");
	}
	type_node* type = ast->d.var_def.type;
	char* name = ast->d.var_def.name;
	expression_node* initializer = ast->d.var_def.initializer;
	if (status.gv_map.find(name) != status.gv_map.end()) {
		throw std::runtime_error(std::string("multiple definition of global variable ") + name);
	}
	int align = type->align;
	if (status.gv_addr % align != 0) {
		status.gv_addr = ((status.gv_addr + align - 1) / align) * align;
	}
	status.gv_map[name] = var_info(status.gv_addr, type);
	status.gv_addr += type->size;
	std::vector<asm_inst> result;
	std::vector<uint32_t> init_values;
	asm_inst_kind inst = EMPTY;
	type_node* element_type;
	int nelem;
	if (type->kind == TYPE_ARRAY) {
		element_type = type->info.element_type;
		if (element_type->kind == TYPE_ARRAY) {
			throw std::runtime_error("array of array not supported");
		}
		nelem = type->size / element_type->size;
	} else {
		element_type = type;
		nelem = 1;
		if (initializer != nullptr) {
			if (initializer->kind == EXPR_INTEGER_LITERAL) {
				init_values.push_back(initializer->info.value);
			} else {
				throw std::runtime_error("unsupported initializer");
			}
		}
	}
	uint32_t mask = 0;
	switch (element_type->size) {
	case 1: inst = DB; mask = UINT32_C(0xff); break;
	case 2: inst = DW; mask = UINT32_C(0xffff); break;
	case 4: inst = DD; mask = UINT32_C(0xffffffff); break;
	default: throw std::runtime_error("unsupported array element size");
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
	codegen_status status;
	status.gv_addr = 0;
	status.gv_map.clear();

	for (size_t i = 0; i < ast->d.array.num; i++) {
		if (ast->d.array.nodes[i]->kind == NODE_VAR_DEF) {
			std::vector<asm_inst> var_inst = codegen_gvar(ast->d.array.nodes[i], status);
			result.insert(result.end(), var_inst.begin(), var_inst.end());
		}
	}

	for (size_t i = 0; i < ast->d.array.num; i++) {
		if (ast->d.array.nodes[i]->kind == NODE_FUNC_DEF) {
			std::vector<asm_inst> func_inst = codegen_func(ast->d.array.nodes[i], status);
			result.insert(result.end(), func_inst.begin(), func_inst.end());
		}
	}

	return result;
}
