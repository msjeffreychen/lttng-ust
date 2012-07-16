/*
 * lttng-filter-validator.c
 *
 * LTTng UST filter bytecode validator.
 *
 * Copyright (C) 2010-2012 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; only
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#define _LGPL_SOURCE
#include <urcu-bp.h>
#include <time.h>
#include "lttng-filter.h"

#include <urcu/rculfhash.h>
#include "lttng-hash-helper.h"

/* merge point table node */
struct lfht_mp_node {
	struct cds_lfht_node node;

	/* Context at merge point */
	struct vreg reg[NR_REG];
	unsigned long target_pc;
};

static unsigned long lttng_hash_seed;
static unsigned int lttng_hash_seed_ready;

static
int lttng_hash_match(struct cds_lfht_node *node, const void *key)
{
	struct lfht_mp_node *mp_node =
		caa_container_of(node, struct lfht_mp_node, node);
	unsigned long key_pc = (unsigned long) key;

	if (mp_node->target_pc == key_pc)
		return 1;
	else
		return 0;
}

static
int merge_point_add(struct cds_lfht *ht, unsigned long target_pc,
		const struct vreg reg[NR_REG])
{
	struct lfht_mp_node *node;
	unsigned long hash = lttng_hash_mix((const void *) target_pc,
				sizeof(target_pc),
				lttng_hash_seed);

	dbg_printf("Filter: adding merge point at offset %lu, hash %lu\n",
			target_pc, hash);
	node = zmalloc(sizeof(struct lfht_mp_node));
	if (!node)
		return -ENOMEM;
	node->target_pc = target_pc;
	memcpy(node->reg, reg, sizeof(node->reg));
	cds_lfht_add(ht, hash, &node->node);
	return 0;
}

/*
 * Number of merge points for hash table size. Hash table initialized to
 * that size, and we do not resize, because we do not want to trigger
 * RCU worker thread execution: fall-back on linear traversal if number
 * of merge points exceeds this value.
 */
#define DEFAULT_NR_MERGE_POINTS		128
#define MIN_NR_BUCKETS			128
#define MAX_NR_BUCKETS			128

static
int bin_op_compare_check(const struct vreg reg[NR_REG], const char *str)
{
	switch (reg[REG_R0].type) {
	default:
		goto error_unknown;

	case REG_STRING:
		switch (reg[REG_R1].type) {
		default:
			goto error_unknown;

		case REG_STRING:
			break;
		case REG_S64:
		case REG_DOUBLE:
			goto error_mismatch;
		}
		break;
	case REG_S64:
	case REG_DOUBLE:
		switch (reg[REG_R1].type) {
		default:
			goto error_unknown;

		case REG_STRING:
			goto error_mismatch;

		case REG_S64:
		case REG_DOUBLE:
			break;
		}
		break;
	}
	return 0;

error_unknown:

	return -EINVAL;
error_mismatch:
	ERR("type mismatch for '%s' binary operator\n", str);
	return -EINVAL;
}

/*
 * Validate bytecode range overflow within the validation pass.
 * Called for each instruction encountered.
 */
static
int bytecode_validate_overflow(struct bytecode_runtime *bytecode,
		void *start_pc, void *pc)
{
	int ret = 0;

	switch (*(filter_opcode_t *) pc) {
	case FILTER_OP_UNKNOWN:
	default:
	{
		ERR("unknown bytecode op %u\n",
			(unsigned int) *(filter_opcode_t *) pc);
		ret = -EINVAL;
		break;
	}

	case FILTER_OP_RETURN:
	{
		if (unlikely(pc + sizeof(struct return_op)
				> start_pc + bytecode->len)) {
			ret = -EINVAL;
		}
		break;
	}

	/* binary */
	case FILTER_OP_MUL:
	case FILTER_OP_DIV:
	case FILTER_OP_MOD:
	case FILTER_OP_PLUS:
	case FILTER_OP_MINUS:
	case FILTER_OP_RSHIFT:
	case FILTER_OP_LSHIFT:
	case FILTER_OP_BIN_AND:
	case FILTER_OP_BIN_OR:
	case FILTER_OP_BIN_XOR:
	{
		ERR("unsupported bytecode op %u\n",
			(unsigned int) *(filter_opcode_t *) pc);
		ret = -EINVAL;
		break;
	}

	case FILTER_OP_EQ:
	case FILTER_OP_NE:
	case FILTER_OP_GT:
	case FILTER_OP_LT:
	case FILTER_OP_GE:
	case FILTER_OP_LE:
	case FILTER_OP_EQ_STRING:
	case FILTER_OP_NE_STRING:
	case FILTER_OP_GT_STRING:
	case FILTER_OP_LT_STRING:
	case FILTER_OP_GE_STRING:
	case FILTER_OP_LE_STRING:
	case FILTER_OP_EQ_S64:
	case FILTER_OP_NE_S64:
	case FILTER_OP_GT_S64:
	case FILTER_OP_LT_S64:
	case FILTER_OP_GE_S64:
	case FILTER_OP_LE_S64:
	case FILTER_OP_EQ_DOUBLE:
	case FILTER_OP_NE_DOUBLE:
	case FILTER_OP_GT_DOUBLE:
	case FILTER_OP_LT_DOUBLE:
	case FILTER_OP_GE_DOUBLE:
	case FILTER_OP_LE_DOUBLE:
	{
		if (unlikely(pc + sizeof(struct binary_op)
				> start_pc + bytecode->len)) {
			ret = -EINVAL;
		}
		break;
	}

	/* unary */
	case FILTER_OP_UNARY_PLUS:
	case FILTER_OP_UNARY_MINUS:
	case FILTER_OP_UNARY_NOT:
	case FILTER_OP_UNARY_PLUS_S64:
	case FILTER_OP_UNARY_MINUS_S64:
	case FILTER_OP_UNARY_NOT_S64:
	case FILTER_OP_UNARY_PLUS_DOUBLE:
	case FILTER_OP_UNARY_MINUS_DOUBLE:
	case FILTER_OP_UNARY_NOT_DOUBLE:
	{
		if (unlikely(pc + sizeof(struct unary_op)
				> start_pc + bytecode->len)) {
			ret = -EINVAL;
		}
		break;
	}

	/* logical */
	case FILTER_OP_AND:
	case FILTER_OP_OR:
	{
		if (unlikely(pc + sizeof(struct logical_op)
				> start_pc + bytecode->len)) {
			ret = -EINVAL;
		}
		break;
	}

	/* load */
	case FILTER_OP_LOAD_FIELD_REF:
	{
		ERR("Unknown field ref type\n");
		ret = -EINVAL;
		break;
	}
	case FILTER_OP_LOAD_FIELD_REF_STRING:
	case FILTER_OP_LOAD_FIELD_REF_SEQUENCE:
	case FILTER_OP_LOAD_FIELD_REF_S64:
	case FILTER_OP_LOAD_FIELD_REF_DOUBLE:
	{
		if (unlikely(pc + sizeof(struct load_op) + sizeof(struct field_ref)
				> start_pc + bytecode->len)) {
			ret = -EINVAL;
		}
		break;
	}

	case FILTER_OP_LOAD_STRING:
	{
		struct load_op *insn = (struct load_op *) pc;
		uint32_t str_len, maxlen;

		if (unlikely(pc + sizeof(struct load_op)
				> start_pc + bytecode->len)) {
			ret = -EINVAL;
			break;
		}

		maxlen = start_pc + bytecode->len - pc - sizeof(struct load_op);
		str_len = strnlen(insn->data, maxlen);
		if (unlikely(str_len >= maxlen)) {
			/* Final '\0' not found within range */
			ret = -EINVAL;
		}
		break;
	}

	case FILTER_OP_LOAD_S64:
	{
		if (unlikely(pc + sizeof(struct load_op) + sizeof(struct literal_numeric)
				> start_pc + bytecode->len)) {
			ret = -EINVAL;
		}
		break;
	}

	case FILTER_OP_LOAD_DOUBLE:
	{
		if (unlikely(pc + sizeof(struct load_op) + sizeof(struct literal_double)
				> start_pc + bytecode->len)) {
			ret = -EINVAL;
		}
		break;
	}

	case FILTER_OP_CAST_TO_S64:
	case FILTER_OP_CAST_DOUBLE_TO_S64:
	case FILTER_OP_CAST_NOP:
	{
		if (unlikely(pc + sizeof(struct cast_op)
				> start_pc + bytecode->len)) {
			ret = -EINVAL;
		}
		break;
	}
	}

	return ret;
}

static
unsigned long delete_all_nodes(struct cds_lfht *ht)
{
	struct cds_lfht_iter iter;
	struct lfht_mp_node *node;
	unsigned long nr_nodes = 0;

	cds_lfht_for_each_entry(ht, &iter, node, node) {
		int ret;

		ret = cds_lfht_del(ht, cds_lfht_iter_get_node(&iter));
		assert(!ret);
		/* note: this hash table is never used concurrently */
		free(node);
		nr_nodes++;
	}
	return nr_nodes;
}

/*
 * Return value:
 * 0: success
 * <0: error
 */
static
int validate_instruction_context(struct bytecode_runtime *bytecode,
		const struct vreg reg[NR_REG],
		void *start_pc,
		void *pc)
{
	int ret = 0;

	switch (*(filter_opcode_t *) pc) {
	case FILTER_OP_UNKNOWN:
	default:
	{
		ERR("unknown bytecode op %u\n",
			(unsigned int) *(filter_opcode_t *) pc);
		ret = -EINVAL;
		goto end;
	}

	case FILTER_OP_RETURN:
	{
		goto end;
	}

	/* binary */
	case FILTER_OP_MUL:
	case FILTER_OP_DIV:
	case FILTER_OP_MOD:
	case FILTER_OP_PLUS:
	case FILTER_OP_MINUS:
	case FILTER_OP_RSHIFT:
	case FILTER_OP_LSHIFT:
	case FILTER_OP_BIN_AND:
	case FILTER_OP_BIN_OR:
	case FILTER_OP_BIN_XOR:
	{
		ERR("unsupported bytecode op %u\n",
			(unsigned int) *(filter_opcode_t *) pc);
		ret = -EINVAL;
		goto end;
	}

	case FILTER_OP_EQ:
	{
		ret = bin_op_compare_check(reg, "==");
		if (ret)
			goto end;
		break;
	}
	case FILTER_OP_NE:
	{
		ret = bin_op_compare_check(reg, "!=");
		if (ret)
			goto end;
		break;
	}
	case FILTER_OP_GT:
	{
		ret = bin_op_compare_check(reg, ">");
		if (ret)
			goto end;
		break;
	}
	case FILTER_OP_LT:
	{
		ret = bin_op_compare_check(reg, "<");
		if (ret)
			goto end;
		break;
	}
	case FILTER_OP_GE:
	{
		ret = bin_op_compare_check(reg, ">=");
		if (ret)
			goto end;
		break;
	}
	case FILTER_OP_LE:
	{
		ret = bin_op_compare_check(reg, "<=");
		if (ret)
			goto end;
		break;
	}

	case FILTER_OP_EQ_STRING:
	case FILTER_OP_NE_STRING:
	case FILTER_OP_GT_STRING:
	case FILTER_OP_LT_STRING:
	case FILTER_OP_GE_STRING:
	case FILTER_OP_LE_STRING:
	{
		if (reg[REG_R0].type != REG_STRING
				|| reg[REG_R1].type != REG_STRING) {
			ERR("Unexpected register type for string comparator\n");
			ret = -EINVAL;
			goto end;
		}
		break;
	}

	case FILTER_OP_EQ_S64:
	case FILTER_OP_NE_S64:
	case FILTER_OP_GT_S64:
	case FILTER_OP_LT_S64:
	case FILTER_OP_GE_S64:
	case FILTER_OP_LE_S64:
	{
		if (reg[REG_R0].type != REG_S64
				|| reg[REG_R1].type != REG_S64) {
			ERR("Unexpected register type for s64 comparator\n");
			ret = -EINVAL;
			goto end;
		}
		break;
	}

	case FILTER_OP_EQ_DOUBLE:
	case FILTER_OP_NE_DOUBLE:
	case FILTER_OP_GT_DOUBLE:
	case FILTER_OP_LT_DOUBLE:
	case FILTER_OP_GE_DOUBLE:
	case FILTER_OP_LE_DOUBLE:
	{
		if ((reg[REG_R0].type != REG_DOUBLE && reg[REG_R0].type != REG_S64)
				|| (reg[REG_R1].type != REG_DOUBLE && reg[REG_R1].type != REG_S64)) {
			ERR("Unexpected register type for double comparator\n");
			ret = -EINVAL;
			goto end;
		}
		if (reg[REG_R0].type != REG_DOUBLE && reg[REG_R1].type != REG_DOUBLE) {
			ERR("Double operator should have at least one double register\n");
			ret = -EINVAL;
			goto end;
		}
		break;
	}

	/* unary */
	case FILTER_OP_UNARY_PLUS:
	case FILTER_OP_UNARY_MINUS:
	case FILTER_OP_UNARY_NOT:
	{
		struct unary_op *insn = (struct unary_op *) pc;

		if (unlikely(insn->reg >= REG_ERROR)) {
			ERR("invalid register %u\n",
				(unsigned int) insn->reg);
			ret = -EINVAL;
			goto end;
		}
		switch (reg[insn->reg].type) {
		default:
			ERR("unknown register type\n");
			ret = -EINVAL;
			goto end;

		case REG_STRING:
			ERR("Unary op can only be applied to numeric or floating point registers\n");
			ret = -EINVAL;
			goto end;
		case REG_S64:
			break;
		case REG_DOUBLE:
			break;
		}
		break;
	}

	case FILTER_OP_UNARY_PLUS_S64:
	case FILTER_OP_UNARY_MINUS_S64:
	case FILTER_OP_UNARY_NOT_S64:
	{
		struct unary_op *insn = (struct unary_op *) pc;

		if (unlikely(insn->reg >= REG_ERROR)) {
			ERR("invalid register %u\n",
				(unsigned int) insn->reg);
			ret = -EINVAL;
			goto end;
		}
		if (reg[insn->reg].type != REG_S64) {
			ERR("Invalid register type\n");
			ret = -EINVAL;
			goto end;
		}
		break;
	}

	case FILTER_OP_UNARY_PLUS_DOUBLE:
	case FILTER_OP_UNARY_MINUS_DOUBLE:
	case FILTER_OP_UNARY_NOT_DOUBLE:
	{
		struct unary_op *insn = (struct unary_op *) pc;

		if (unlikely(insn->reg >= REG_ERROR)) {
			ERR("invalid register %u\n",
				(unsigned int) insn->reg);
			ret = -EINVAL;
			goto end;
		}
		if (reg[insn->reg].type != REG_DOUBLE) {
			ERR("Invalid register type\n");
			ret = -EINVAL;
			goto end;
		}
		break;
	}

	/* logical */
	case FILTER_OP_AND:
	case FILTER_OP_OR:
	{
		struct logical_op *insn = (struct logical_op *) pc;

		if (reg[REG_R0].type != REG_S64) {
			ERR("Logical comparator expects S64 register\n");
			ret = -EINVAL;
			goto end;
		}

		dbg_printf("Validate jumping to bytecode offset %u\n",
			(unsigned int) insn->skip_offset);
		if (unlikely(start_pc + insn->skip_offset <= pc)) {
			ERR("Loops are not allowed in bytecode\n");
			ret = -EINVAL;
			goto end;
		}
		break;
	}

	/* load */
	case FILTER_OP_LOAD_FIELD_REF:
	{
		ERR("Unknown field ref type\n");
		ret = -EINVAL;
		goto end;
	}
	case FILTER_OP_LOAD_FIELD_REF_STRING:
	case FILTER_OP_LOAD_FIELD_REF_SEQUENCE:
	{
		struct load_op *insn = (struct load_op *) pc;
		struct field_ref *ref = (struct field_ref *) insn->data;

		if (unlikely(insn->reg >= REG_ERROR)) {
			ERR("invalid register %u\n",
				(unsigned int) insn->reg);
			ret = -EINVAL;
			goto end;
		}
		dbg_printf("Validate load field ref offset %u type string\n",
			ref->offset);
		break;
	}
	case FILTER_OP_LOAD_FIELD_REF_S64:
	{
		struct load_op *insn = (struct load_op *) pc;
		struct field_ref *ref = (struct field_ref *) insn->data;

		if (unlikely(insn->reg >= REG_ERROR)) {
			ERR("invalid register %u\n",
				(unsigned int) insn->reg);
			ret = -EINVAL;
			goto end;
		}
		dbg_printf("Validate load field ref offset %u type s64\n",
			ref->offset);
		break;
	}
	case FILTER_OP_LOAD_FIELD_REF_DOUBLE:
	{
		struct load_op *insn = (struct load_op *) pc;
		struct field_ref *ref = (struct field_ref *) insn->data;

		if (unlikely(insn->reg >= REG_ERROR)) {
			ERR("invalid register %u\n",
				(unsigned int) insn->reg);
			ret = -EINVAL;
			goto end;
		}
		dbg_printf("Validate load field ref offset %u type double\n",
			ref->offset);
		break;
	}

	case FILTER_OP_LOAD_STRING:
	{
		struct load_op *insn = (struct load_op *) pc;

		if (unlikely(insn->reg >= REG_ERROR)) {
			ERR("invalid register %u\n",
				(unsigned int) insn->reg);
			ret = -EINVAL;
			goto end;
		}
		break;
	}

	case FILTER_OP_LOAD_S64:
	{
		struct load_op *insn = (struct load_op *) pc;

		if (unlikely(insn->reg >= REG_ERROR)) {
			ERR("invalid register %u\n",
				(unsigned int) insn->reg);
			ret = -EINVAL;
			goto end;
		}
		break;
	}

	case FILTER_OP_LOAD_DOUBLE:
	{
		struct load_op *insn = (struct load_op *) pc;

		if (unlikely(insn->reg >= REG_ERROR)) {
			ERR("invalid register %u\n",
				(unsigned int) insn->reg);
			ret = -EINVAL;
			goto end;
		}
		break;
	}

	case FILTER_OP_CAST_TO_S64:
	case FILTER_OP_CAST_DOUBLE_TO_S64:
	{
		struct cast_op *insn = (struct cast_op *) pc;

		if (unlikely(insn->reg >= REG_ERROR)) {
			ERR("invalid register %u\n",
				(unsigned int) insn->reg);
			ret = -EINVAL;
			goto end;
		}
		switch (reg[insn->reg].type) {
		default:
			ERR("unknown register type\n");
			ret = -EINVAL;
			goto end;

		case REG_STRING:
			ERR("Cast op can only be applied to numeric or floating point registers\n");
			ret = -EINVAL;
			goto end;
		case REG_S64:
			break;
		case REG_DOUBLE:
			break;
		}
		if (insn->op == FILTER_OP_CAST_DOUBLE_TO_S64) {
			if (reg[insn->reg].type != REG_DOUBLE) {
				ERR("Cast expects double\n");
				ret = -EINVAL;
				goto end;
			}
		}
		break;
	}
	case FILTER_OP_CAST_NOP:
	{
		break;
	}

	}
end:
	return ret;
}

/*
 * Return value:
 * 0: success
 * <0: error
 */
static
int validate_instruction_all_contexts(struct bytecode_runtime *bytecode,
		struct cds_lfht *merge_points,
		const struct vreg reg[NR_REG],
		void *start_pc,
		void *pc)
{
	int ret;
	unsigned long target_pc = pc - start_pc;
	struct cds_lfht_iter iter;
	struct cds_lfht_node *node;
	unsigned long hash;

	/* Validate the context resulting from the previous instruction */
	ret = validate_instruction_context(bytecode, reg, start_pc, pc);
	if (ret)
		return ret;

	/* Validate merge points */
	hash = lttng_hash_mix((const void *) target_pc, sizeof(target_pc),
			lttng_hash_seed);
	cds_lfht_for_each_duplicate(merge_points, hash, lttng_hash_match,
			(const void *) target_pc, &iter, node) {
		struct lfht_mp_node *mp_node =
			caa_container_of(node, struct lfht_mp_node, node);
		
		dbg_printf("Filter: validate merge point at offset %lu\n",
				target_pc);
		ret = validate_instruction_context(bytecode, mp_node->reg,
				start_pc, pc);
		if (ret)
			return ret;
		/* Once validated, we can remove the merge point */
		dbg_printf("Filter: remove one merge point at offset %lu\n",
				target_pc);
		ret = cds_lfht_del(merge_points, node);
		assert(!ret);
	}
	return 0;
}

/*
 * Return value:
 * >0: going to next insn.
 * 0: success, stop iteration.
 * <0: error
 */
static
int exec_insn(struct bytecode_runtime *bytecode,
		struct cds_lfht *merge_points,
		struct vreg reg[NR_REG],
		void **_next_pc,
		void *pc)
{
	int ret = 1;
	void *next_pc = *_next_pc;

	switch (*(filter_opcode_t *) pc) {
	case FILTER_OP_UNKNOWN:
	default:
	{
		ERR("unknown bytecode op %u\n",
			(unsigned int) *(filter_opcode_t *) pc);
		ret = -EINVAL;
		goto end;
	}

	case FILTER_OP_RETURN:
	{
		ret = 0;
		goto end;
	}

	/* binary */
	case FILTER_OP_MUL:
	case FILTER_OP_DIV:
	case FILTER_OP_MOD:
	case FILTER_OP_PLUS:
	case FILTER_OP_MINUS:
	case FILTER_OP_RSHIFT:
	case FILTER_OP_LSHIFT:
	case FILTER_OP_BIN_AND:
	case FILTER_OP_BIN_OR:
	case FILTER_OP_BIN_XOR:
	{
		ERR("unsupported bytecode op %u\n",
			(unsigned int) *(filter_opcode_t *) pc);
		ret = -EINVAL;
		goto end;
	}

	case FILTER_OP_EQ:
	case FILTER_OP_NE:
	case FILTER_OP_GT:
	case FILTER_OP_LT:
	case FILTER_OP_GE:
	case FILTER_OP_LE:
	case FILTER_OP_EQ_STRING:
	case FILTER_OP_NE_STRING:
	case FILTER_OP_GT_STRING:
	case FILTER_OP_LT_STRING:
	case FILTER_OP_GE_STRING:
	case FILTER_OP_LE_STRING:
	case FILTER_OP_EQ_S64:
	case FILTER_OP_NE_S64:
	case FILTER_OP_GT_S64:
	case FILTER_OP_LT_S64:
	case FILTER_OP_GE_S64:
	case FILTER_OP_LE_S64:
	{
		reg[REG_R0].type = REG_S64;
		next_pc += sizeof(struct binary_op);
		break;
	}

	case FILTER_OP_EQ_DOUBLE:
	case FILTER_OP_NE_DOUBLE:
	case FILTER_OP_GT_DOUBLE:
	case FILTER_OP_LT_DOUBLE:
	case FILTER_OP_GE_DOUBLE:
	case FILTER_OP_LE_DOUBLE:
	{
		reg[REG_R0].type = REG_DOUBLE;
		next_pc += sizeof(struct binary_op);
		break;
	}

	/* unary */
	case FILTER_OP_UNARY_PLUS:
	case FILTER_OP_UNARY_MINUS:
	case FILTER_OP_UNARY_NOT:
	case FILTER_OP_UNARY_PLUS_S64:
	case FILTER_OP_UNARY_MINUS_S64:
	case FILTER_OP_UNARY_NOT_S64:
	{
		reg[REG_R0].type = REG_S64;
		next_pc += sizeof(struct unary_op);
		break;
	}

	case FILTER_OP_UNARY_PLUS_DOUBLE:
	case FILTER_OP_UNARY_MINUS_DOUBLE:
	case FILTER_OP_UNARY_NOT_DOUBLE:
	{
		reg[REG_R0].type = REG_DOUBLE;
		next_pc += sizeof(struct unary_op);
		break;
	}

	/* logical */
	case FILTER_OP_AND:
	case FILTER_OP_OR:
	{
		struct logical_op *insn = (struct logical_op *) pc;
		int merge_ret;

		/* Add merge point to table */
		merge_ret = merge_point_add(merge_points, insn->skip_offset, reg);
		if (merge_ret) {
			ret = merge_ret;
			goto end;
		}
		/* Continue to next instruction */
		next_pc += sizeof(struct logical_op);
		break;
	}

	/* load */
	case FILTER_OP_LOAD_FIELD_REF:
	{
		ERR("Unknown field ref type\n");
		ret = -EINVAL;
		goto end;
	}
	case FILTER_OP_LOAD_FIELD_REF_STRING:
	case FILTER_OP_LOAD_FIELD_REF_SEQUENCE:
	{
		struct load_op *insn = (struct load_op *) pc;

		reg[insn->reg].type = REG_STRING;
		reg[insn->reg].literal = 0;
		next_pc += sizeof(struct load_op) + sizeof(struct field_ref);
		break;
	}
	case FILTER_OP_LOAD_FIELD_REF_S64:
	{
		struct load_op *insn = (struct load_op *) pc;

		reg[insn->reg].type = REG_S64;
		reg[insn->reg].literal = 0;
		next_pc += sizeof(struct load_op) + sizeof(struct field_ref);
		break;
	}
	case FILTER_OP_LOAD_FIELD_REF_DOUBLE:
	{
		struct load_op *insn = (struct load_op *) pc;

		reg[insn->reg].type = REG_DOUBLE;
		reg[insn->reg].literal = 0;
		next_pc += sizeof(struct load_op) + sizeof(struct field_ref);
		break;
	}

	case FILTER_OP_LOAD_STRING:
	{
		struct load_op *insn = (struct load_op *) pc;

		reg[insn->reg].type = REG_STRING;
		reg[insn->reg].literal = 1;
		next_pc += sizeof(struct load_op) + strlen(insn->data) + 1;
		break;
	}

	case FILTER_OP_LOAD_S64:
	{
		struct load_op *insn = (struct load_op *) pc;

		reg[insn->reg].type = REG_S64;
		reg[insn->reg].literal = 1;
		next_pc += sizeof(struct load_op)
				+ sizeof(struct literal_numeric);
		break;
	}

	case FILTER_OP_LOAD_DOUBLE:
	{
		struct load_op *insn = (struct load_op *) pc;

		reg[insn->reg].type = REG_DOUBLE;
		reg[insn->reg].literal = 1;
		next_pc += sizeof(struct load_op)
				+ sizeof(struct literal_double);
		break;
	}

	case FILTER_OP_CAST_TO_S64:
	case FILTER_OP_CAST_DOUBLE_TO_S64:
	{
		struct cast_op *insn = (struct cast_op *) pc;

		reg[insn->reg].type = REG_S64;
		next_pc += sizeof(struct cast_op);
		break;
	}
	case FILTER_OP_CAST_NOP:
	{
		next_pc += sizeof(struct cast_op);
		break;
	}

	}
end:
	*_next_pc = next_pc;
	return ret;
}

/*
 * Never called concurrently (hash seed is shared).
 */
int lttng_filter_validate_bytecode(struct bytecode_runtime *bytecode)
{
	struct cds_lfht *merge_points;
	void *pc, *next_pc, *start_pc;
	int ret = -EINVAL;
	struct vreg reg[NR_REG];
	int i;

	for (i = 0; i < NR_REG; i++) {
		reg[i].type = REG_TYPE_UNKNOWN;
		reg[i].literal = 0;
	}

	if (!lttng_hash_seed_ready) {
		lttng_hash_seed = time(NULL);
		lttng_hash_seed_ready = 1;
	}
	/*
	 * Note: merge_points hash table used by single thread, and
	 * never concurrently resized. Therefore, we can use it without
	 * holding RCU read-side lock and free nodes without using
	 * call_rcu.
	 */
	merge_points = cds_lfht_new(DEFAULT_NR_MERGE_POINTS,
			MIN_NR_BUCKETS, MAX_NR_BUCKETS,
			0, NULL);
	if (!merge_points) {
		ERR("Error allocating hash table for bytecode validation\n");
		return -ENOMEM;
	}
	start_pc = &bytecode->data[0];
	for (pc = next_pc = start_pc; pc - start_pc < bytecode->len;
			pc = next_pc) {
		if (bytecode_validate_overflow(bytecode, start_pc, pc) != 0) {
			ERR("filter bytecode overflow\n");
			ret = -EINVAL;
			goto end;
		}
		dbg_printf("Validating op %s (%u)\n",
			print_op((unsigned int) *(filter_opcode_t *) pc),
			(unsigned int) *(filter_opcode_t *) pc);

		/*
		 * For each instruction, validate the current context
		 * (traversal of entire execution flow), and validate
		 * all 	merge points targeting this instruction.
		 */
		ret = validate_instruction_all_contexts(bytecode, merge_points,
					reg, start_pc, pc);
		if (ret)
			goto end;
		ret = exec_insn(bytecode, merge_points, reg, &next_pc, pc);
		if (ret <= 0)
			goto end;
	}
end:
	if (delete_all_nodes(merge_points)) {
		if (!ret) {
			ERR("Unexpected merge points\n");
			ret = -EINVAL;
		}
	}
	if (cds_lfht_destroy(merge_points, NULL)) {
		ERR("Error destroying hash table\n");
	}
	return ret;
}
