#include <stdio.h>
#include "ast.h"

int main(void) {
	ast_node* ast;
	ast = build_ast(stdin);
	if (ast == NULL) return 1;
	return 0;
}
