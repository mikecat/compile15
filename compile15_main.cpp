#include <cstdio>
#include <vector>
#include <string>
#include "ast.h"
#include "asm.hpp"
#include "codegen.hpp"

int main(void) {
	ast_node* ast;
	ast = build_ast(stdin);
	if (ast == NULL) return 1;
	try {
		std::vector<asm_inst> code = codegen(ast);
		codegen_clean(code);
		for (auto itr = code.begin(); itr != code.end(); itr++) {
			std::string inst_str = itr->to_string();
			if (itr->kind != LABEL && inst_str.length() > 0) printf("\t");
			printf("%s\n", inst_str.c_str());
		}
	} catch (codegen_error e) {
		fprintf(stderr, "code generation error: %s\n", e.what());
		return 1;
	}
	return 0;
}
