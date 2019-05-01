#include <string>
#include <vector>
#include <set>
#include <map>
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

// 連続した自動生成ラベルを1個にまとめる
static bool merge_generated_labels(std::vector<asm_inst>& insts) {
	bool progress_exists = false;
	std::map<std::string, std::string> rewrite_map;
	// 書き換え関係を調査する
	bool rewriting = false;
	std::string rewrite_to = "";
	for (auto itr = insts.begin(); itr != insts.end(); itr++) {
		if (itr->kind == LABEL && itr->label[0] == '_' && itr->label[1] == '_') {
			// 自動生成ラベル
			if (rewriting) {
				// 2番目以降なら、最初のラベルに書き換える指示を出す
				rewrite_map[itr->label] = rewrite_to;
			} else {
				// 最初なら、書き換え先として登録する
				rewriting = true;
				rewrite_to = itr->label;
			}
		} else if (itr->kind != EMPTY) {
			// 自動生成ラベル以外のものが来たら、書き換えを止める
			rewriting = false;
		}
	}
	// 書き換えを実行する
	for (auto itr = insts.begin(); itr != insts.end(); itr++) {
		if (itr->label != "" &&
		(itr->kind == JCC || itr->kind == JMP_DIRECT || itr->kind == CALL_DIRECT)) {
			if (rewrite_map.find(itr->label) != rewrite_map.end()) {
				itr->label = rewrite_map[itr->label];
				progress_exists = true;
			}
		}
	}
	return progress_exists;
}

// 直後にGOTOやRETが来る自動生成ラベルを簡約する
static bool fold_goto(std::vector<asm_inst>& insts) {
	bool progress_exists = false;
	std::map<std::string, std::string> goto_map;
	std::set<std::string> ret_set;
	// 書き換え関係を調査する
	bool after_label = false;
	std::string current_label = "";
	for (auto itr = insts.begin(); itr != insts.end(); itr++) {
		if (itr->kind == LABEL && itr->label[0] == '_' && itr->label[1] == '_') {
			// 自動生成ラベル
			after_label = true;
			current_label = itr->label;
		} else if (itr->kind != EMPTY) {
			if (after_label) {
				if (itr->kind == JMP_DIRECT) {
					goto_map[current_label] = itr->label;
				} else if (itr->kind == RET) {
					ret_set.insert(current_label);
				}
			}
			after_label = false;
		}
	}
	// 書き換えを実行する
	for (auto itr = insts.begin(); itr != insts.end(); itr++) {
		if (itr->label != "" &&
		(itr->kind == JCC || itr->kind == JMP_DIRECT || itr->kind == CALL_DIRECT)) {
			if (goto_map.find(itr->label) != goto_map.end()) {
				itr->label = goto_map[itr->label];
				progress_exists = true;
			} else if (itr->kind == JMP_DIRECT && ret_set.find(itr->label) != ret_set.end()) {
				itr->kind = RET;
				progress_exists = true;
			}
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
		if (merge_generated_labels(insts)) progress_exists = true;
		if (fold_goto(insts)) progress_exists = true;
	} while (progress_exists);
}
