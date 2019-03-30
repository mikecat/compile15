#ifndef CODEGEN_INTERNAL_HPP_GUARD_B9327909_5E6A_42DA_93A1_947E11E2E0AB
#define CODEGEN_INTERNAL_HPP_GUARD_B9327909_5E6A_42DA_93A1_947E11E2E0AB

#include <map>
#include <vector>
#include <string>
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
	int next_label;
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

	int return_label;
	type_node* return_type;

	struct regen_checkpoint {
		int next_label;
		int registers_written;

		regen_checkpoint(int nl = 0, int rw = 0) : next_label(nl), registers_written(rw) {}
	};
	regen_checkpoint save_checkpoint() const {
		return regen_checkpoint(next_label, registers_written);
	}
	void load_checkpoint(const regen_checkpoint& cp) {
		next_label = cp.next_label;
		registers_written = cp.registers_written;
	}
};

struct codegen_expr_result {
	std::vector<asm_inst> insts;
	int result_reg;

	codegen_expr_result() {}
	codegen_expr_result(const std::vector<asm_inst>& insts_, int result_reg_ = -1) :
		insts(insts_), result_reg(result_reg_) {}
};

struct offset_fold_result {
	var_info* vinfo;
	int additional_offset;
	expression_node* vnode;
	expression_node* offset_node;
	bool negate_offset_node;

	offset_fold_result(var_info* vinfo_ = nullptr, int additional_offset_ = 0,
		expression_node* vnode_ = nullptr, expression_node* offset_node_ = nullptr,
		bool negate_offset_node_ = false) : vinfo(vinfo_), additional_offset(additional_offset_),
			vnode(vnode_), offset_node(offset_node_), negate_offset_node(negate_offset_node_) {}
};

struct codegen_mem_cache {
	int size;
	bool is_signed;
	bool is_register;
	bool use_two_params;
	asm_inst_kind read_inst, write_inst;
	uint32_t mem_param1, mem_param2;
	int regs_in_cache;
};

struct codegen_mem_result {
	codegen_expr_result code;
	codegen_mem_cache cache;

	codegen_mem_result() {}
	codegen_mem_result(const codegen_expr_result& e, const codegen_mem_cache& c) :
		code(e), cache(c) {}
};

// codegen.cpp

// ラベルIDからラベル(文字列)を作成する
std::string get_label(int id);
// 指定された数が2の非負整数乗ならその指数を返し、そうでなければ負の数を返す
int get_two_pow_num(uint32_t value);
// グローバル変数のコードを生成する
std::vector<asm_inst> codegen_gvar(ast_node* ast, codegen_status& status);
// 指定のレジスタに指定の数を置くコードを生成する
std::vector<asm_inst> codegen_put_number(int dest_reg, uint32_t value);
// グローバル変数アクセス用のレジスタを設定する
std::vector<asm_inst> codegen_set_gv_access_register(int dest_reg, int src_reg,
	int base_address, codegen_status& status);
// 関数定義のコードを生成する
std::vector<asm_inst> codegen_func(ast_node* ast, codegen_status& status);
// 全体のコードを生成する
std::vector<asm_inst> codegen(ast_node* ast);
// 生成したコードを改善する
void codegen_clean(std::vector<asm_inst>& insts);

// codegen_block_pre.cpp

// 今のブロックに変数を登録し、登録した変数のオフセットを返す
int codegen_register_variable(ast_node* def_node, codegen_status& status,
	bool argument_mode, bool pragma_use_register, int pragma_use_register_id);
// ブロックの前処理を行う
void codegen_preprocess_block(ast_node* ast, codegen_status& status);

// codegen_expr_pre.cpp

// 自動挿入用の演算子を自動挿入する
void codegen_add_auto_operator(operator_type op, int pos, expression_node** expr);
// 式中の識別子を解決する
void codegen_resolve_identifier_expr(expression_node* expr, int lineno, codegen_status& status);
// 式評価のスケジューリング用のヒントを設定する
expr_info* get_operator_hint(expression_node* expr, int lineno);
// スケジューリング用ヒントを比較する
int cmp_expr_info(expr_info* a, expr_info* b);
// 式の前処理を行う
void codegen_preprocess_expr(expression_node* expr, int lineno, codegen_status& status);

// codegen_block.cpp

// 文のコード生成を行う
std::vector<asm_inst> codegen_statement(ast_node* ast, codegen_status& status);

// codegen_expr.cpp

// 使えるレジスタの中から使うレジスタを適当に選ぶ
int get_reg_to_use(int lineno, int regs_available, bool prefer_callee_save);
// 指定のノードのポインタを、一発でメモリアクセスできる形で表そうとする
offset_fold_result* offset_fold(expression_node* node);
// キャッシュを用いたメモリ/レジスタ変数アクセスのコード生成を行う
codegen_expr_result codegen_mem_from_cache(const codegen_mem_cache& cache, int lineno,
	int input_or_result_prefer_reg, bool is_write,
	bool prefer_callee_save, int regs_available, codegen_status& status);
// メモリアクセス(レジスタ変数を含む)のコード生成を行う
codegen_mem_result codegen_mem(expression_node* expr, offset_fold_result* ofr, int lineno,
	expression_node* value_node, bool is_write, bool preserve_cache, bool prefer_callee_save,
	int result_prefer_reg, int regs_available, int stack_extra_offset, codegen_status& status);
// 式のコード生成を行う
codegen_expr_result codegen_expr(expression_node* expr, int lineno, bool want_result, bool prefer_callee_save,
	int result_prefer_reg, int regs_available, int stack_extra_offset, codegen_status& status);
// 条件分岐のコード生成を行う
std::vector<asm_inst> codegen_conditional_jump(expression_node* expr, int lineno,
	const std::string& dest_label, bool jump_if_true,
	int regs_available, int stack_extra_offset, codegen_status& status);

#endif
