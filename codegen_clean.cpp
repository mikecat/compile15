#include <string>
#include <vector>
#include <set>
#include "codegen.hpp"
#include "codegen_internal.hpp"

// 直後のラベルへのジャンプを削除する
static bool remove_jump_to_next(std::vector<asm_inst>& insts) {
	bool progress_exists = false;
	for (auto itr = insts.begin(); itr != insts.end();) {
		bool to_delete = false;
		if (itr->kind == JCC || itr->kind == JMP_DIRECT) {
			auto itr2 = itr;
			for (itr2++; itr2 != insts.end(); itr2++) {
				if (itr2->kind == LABEL) {
					// ラベルなので、ジャンプ先かをチェックする
					if (itr->label == itr2->label) {
						to_delete = true;
						break;
					}
				} else if (itr2->kind != EMPTY) {
					// ラベルと空行以外に当たった = 削除対象ではない
					break;
				}
			}
		}
		if (to_delete) {
			if (itr->comment == "") {
				itr = insts.erase(itr);
			} else {
				itr->kind = EMPTY; // コメントがある場合、コメントだけ残す
				itr++;
			}
			progress_exists = true;
		} else {
			itr++;
		}
	}
	return progress_exists;
}

// 使われていない自動生成ラベルを削除する
static bool remove_unused_generated_labels(std::vector<asm_inst>& insts) {
	bool progress_exists = false;
	std::set<std::string> used_labels;
	for (auto itr = insts.begin(); itr != insts.end(); itr++) {
		if (itr->label != "" &&
		(itr->kind == JCC || itr->kind == JMP_DIRECT || itr->kind == CALL_DIRECT)) {
			used_labels.insert(itr->label);
		}
	}
	for (auto itr = insts.begin(); itr != insts.end();) {
		if (itr->kind == LABEL && itr->label[0] == '_' && itr->label[1] == '_' &&
		used_labels.find(itr->label) == used_labels.end()) {
			// 削除する
			if (itr->comment == "") {
				itr = insts.erase(itr);
			} else {
				itr->kind = EMPTY; // コメントがある場合、コメントだけ残す
				itr++;
			}
			progress_exists = true;
		} else {
			itr++;
		}
	}
	return progress_exists;
}

// 生成したコードを改善する
void codegen_clean(std::vector<asm_inst>& insts) {
	bool progress_exists;
	do {
		progress_exists = false;
		if (remove_jump_to_next(insts)) progress_exists = true;
		if (remove_unused_generated_labels(insts)) progress_exists = true;
	} while (progress_exists);
}
