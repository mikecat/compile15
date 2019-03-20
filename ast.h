#ifndef AST_H_GUARD_C9EA5AE0_9410_4E26_A081_84B8E58D4CF1
#define AST_H_GUARD_C9EA5AE0_9410_4E26_A081_84B8E58D4CF1

#include <stdio.h>
#include <stdint.h>

typedef enum {
	TYPE_INTEGER,
	TYPE_POINTER,
	TYPE_ARRAY,
	TYPE_FUNCTION,
	TYPE_VOID
} type_kind;

typedef struct type_node {
	type_kind kind;
	int size;
	int align;
	union {
		int is_signed; // TYPE_INTEGER
		struct type_node* target_type; // TYPE_POINTER
		struct type_node* element_type; // TYPE_ARRAY
		struct {
			struct type_node* return_type;
			int arg_num; // -1 : 不定
			struct type_node** arg_types;
		} f; // TYPE_FUNCTION
	} info;
} type_node;

typedef enum {
	EXPR_INTEGER_LITERAL,
	EXPR_IDENTIFIER,
	EXPR_OPERATOR
} expression_type;

typedef enum {
	// 単項演算子
	OP_NONE, // 式のトップレベル
	OP_PARENTHESIS,
	OP_FUNC_CALL_NOARGS,
	OP_POST_INC, OP_POST_DEC, OP_PRE_INC, OP_PRE_DEC,
	OP_ADDRESS, OP_INDIRECTION,
	OP_PLUS, OP_NEG, OP_NOT, OP_LNOT,
	OP_SIZEOF,
	OP_CAST,
	// 単項演算子(自動挿入用)
	OP_ARRAY_TO_POINTER, OP_FUNC_TO_FPTR, OP_READ_VALUE,
	OP_DUMMY_BINARY_START, // 二項演算子
	OP_ARRAY_REF, OP_FUNC_CALL,
	OP_MUL, OP_DIV, OP_MOD, OP_ADD, OP_SUB, OP_SHL, OP_SHR,
	OP_LESS, OP_GREATER, OP_LESS_EQUAL, OP_GREATER_EQUAL,
	OP_EQUAL, OP_NOT_EQUAL,
	OP_AND, OP_XOR, OP_OR, OP_LAND, OP_LOR,
	OP_ASSIGN,
	OP_MUL_ASSIGN, OP_DIV_ASSIGN, OP_MOD_ASSIGN, OP_ADD_ASSIGN, OP_SUB_ASSIGN,
	OP_SHL_ASSIGN, OP_SHR_ASSIGN, OP_AND_ASSIGN, OP_XOR_ASSIGN, OP_OR_ASSIGN,
	OP_COMMA,
	OP_DUMMY_TERNARY_START, // 三項演算子
	OP_COND
} operator_type;

struct var_info;
struct expr_info;
typedef struct expression_node {
	expression_type kind;
	type_node* type;
	int is_variable; // lvalueか
	struct expr_info* hint; // スケジューリング用ヒント
	union {
		uint32_t value; // EXPR_INTEGER_LITERAL
		struct {
			char* name;
			struct var_info* info;
		} ident; // EXPR_IDENTIFIER
		struct {
			operator_type kind;
			struct expression_node* operands[3];
			type_node* cast_to;
		} op; // EXPR_OPERATOR
	} info;
} expression_node;

typedef enum {
	NODE_ARRAY,
	NODE_VAR_DEFINE,
	NODE_FUNC_DEFINE,
	NODE_ARGUMENT,
	NODE_EXPR,
	NODE_EMPTY,
	NODE_PRAGMA,
	NODE_CONTROL_IDENTIFIER,
	NODE_CONTROL_INTEGER,
	NODE_RETURN
} node_kind;

typedef struct ast_node {
	node_kind kind;
	int lineno;
	union {
		struct {
			size_t num;
			struct ast_node** nodes;
		} array; // NODE_ARRAY, NODE_PRAGMA
		struct {
			type_node* type;
			char* name;
			int is_register;
			struct expression_node* initializer;
			struct var_info* info;
		} var_def;// NODE_VAR_DEFINE
		struct {
			type_node* return_type;
			char* name;
			struct ast_node* arguments;
			struct ast_node* body;
		} func_def; // NODE_FUNC_DEFINE
		struct {
			type_node* type;
			char* name;
			int is_register;
			struct ast_node* pragmas;
		} arg;// NODE_ARGUMENT
		struct {
			expression_node* expression;
		} expr; // NODE_EXPR
		struct {
			char* name;
		} identifier; // NODE_CONTROL_IDENTIFIER
		struct {
			uint32_t value;
		} integer; // NODE_CONTROL_INTEGER
		struct {
			expression_node* ret_expression;
		} ret; // NODE_RETURN
	} d;
} ast_node;

typedef struct ast_chain_node {
	ast_node* node;
	struct ast_chain_node* next;
} ast_chain_node;

#ifdef __cplusplus
extern "C" {
#endif

// compile15.y
ast_node* build_ast(FILE* fp);

// ast.c
ast_node* new_ast_node(node_kind kind, int lineno);
ast_chain_node* new_chain_node(ast_chain_node* next, ast_node* element);
ast_node* ast_chain_to_array(ast_chain_node* chain, int lineno); // もとのchainは開放する

type_node* new_prim_type(int size, int is_signed);
type_node* new_ptr_type(type_node* target_type);
type_node* new_array_type(int nelem, type_node* element_type);
type_node* new_function_type(type_node* return_type, ast_node* args_array);
type_node* new_void_type(void);
type_node* integer_promotion(type_node* type);
type_node* usual_arithmetic_conversion(type_node* t1, type_node* t2);
int type_is_compatible(type_node* t1, type_node* t2);

expression_node* new_integer_literal(uint32_t value, int is_signed);
expression_node* new_expr_identifier(char* name);
expression_node* new_operator(operator_type op, ...);
void set_operator_expression_type(expression_node* node);
expression_node* constfold(expression_node* node);

#ifdef __cplusplus
}
#endif

#endif
