#ifndef CODEGEN_HPP_GUARD_D5DC1A12_FF82_4030_8283_2BF0EE26F701
#define CODEGEN_HPP_GUARD_D5DC1A12_FF82_4030_8283_2BF0EE26F701

#include <vector>
#include <string>
#include <stdexcept>
#include "ast.h"
#include "asm.hpp"

std::vector<asm_inst> codegen(ast_node* ast);
void codegen_clean(std::vector<asm_inst>& insts);

class codegen_error : public std::runtime_error {
	static std::string build_message(int lineno, std::string message);
public:
	codegen_error(int lineno, const std::string& message) :
		std::runtime_error(build_message(lineno, message)) {}
};

#endif
