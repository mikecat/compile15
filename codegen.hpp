#ifndef CODEGEN_HPP_GUARD_D5DC1A12_FF82_4030_8283_2BF0EE26F701
#define CODEGEN_HPP_GUARD_D5DC1A12_FF82_4030_8283_2BF0EE26F701

#include <vector>
#include "ast.h"
#include "asm.hpp"

std::vector<asm_inst> codegen(ast_node* ast);

#endif
