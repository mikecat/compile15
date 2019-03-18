#ifndef CODEGEN_INTERNAL_HPP_GUARD_B9327909_5E6A_42DA_93A1_947E11E2E0AB
#define CODEGEN_INTERNAL_HPP_GUARD_B9327909_5E6A_42DA_93A1_947E11E2E0AB

#include <map>
#include <vector>
#include "ast.h"
#include "codegen.hpp"

struct var_info {
	int offset;
	type_node* type;
	bool is_global;
	bool is_register;
	var_info(int offset_ = 0, type_node* type_ = nullptr, bool isg = false, bool isr = false) :
		offset(offset_), type(type_), is_global(isg), is_register(isr) {}
};

struct expr_info {
	int num_regs_to_use; // 使うレジスタの数(caller-saveやspillを考慮しない近似値)
	bool func_call_exists; // 関数呼び出しがあるか(caller-saveが発生するか)
	expr_info(int nregs = 0, bool fc = false) : num_regs_to_use(nregs), func_call_exists(fc) {}
};

struct codegen_status {
	// global
	int base_address;
	bool old_entry_exists;
	bool old_single_entry_exists;
	std::string old_single_entry_name;
	int gv_offset;
	bool gv_exists;
	// global + function-local
	std::vector<std::map<std::string, var_info*> > var_maps;
	// function-local (specified from global)
	bool entry_function;
	bool old_entry;
	bool old_single_entry;

	// function-local
	int lv_mem_size;
	std::vector<int> lv_mem_offset;
	int lv_reg_size;
	std::vector<int> lv_reg_offset;
	std::vector<int> lv_reg_assign;

	bool call_exists;
	bool gv_access_exists;
	int gv_access_register;
	int registers_written;
	int registers_reserved;
};

// codegen.cpp

// グローバル変数のコードを生成する
std::vector<asm_inst> codegen_gvar(ast_node* ast, codegen_status& status);
// 今のブロックに変数を登録し、登録した変数のオフセットを返す
int codegen_register_variable(ast_node* def_node, codegen_status& status,
	bool argument_mode, bool pragma_use_register, int pragma_use_register_id);
// ブロックの前処理を行う
void codegen_preprocess_block(ast_node* ast, codegen_status& status);
// 指定のレジスタに指定の数を置くコードを生成する
std::vector<asm_inst> codegen_put_number(int dest_reg, uint32_t value);
// グローバル変数アクセス用のレジスタを設定する
std::vector<asm_inst> codegen_set_gv_access_register(int dest_reg, int src_reg,
	int base_address, codegen_status& status);
// 関数定義のコードを生成する
std::vector<asm_inst> codegen_func(ast_node* ast, codegen_status& status);
// 全体のコードを生成する
std::vector<asm_inst> codegen(ast_node* ast);

// codegen_expr_pre.cpp

// 自動挿入用の演算子を自動挿入する
void codegen_add_auto_operator(operator_type op, int pos, expression_node** expr);
// 式中の識別子を解決する
void codegen_resolve_identifier_expr(expression_node* expr, int lineno, codegen_status& status);
// 式評価のスケジューリング用のヒントを設定する
expr_info* get_operator_hint(expression_node* expr, int lineno);
// 式の前処理を行う
void codegen_preprocess_expr(expression_node* expr, int lineno, codegen_status& status);

#endif
