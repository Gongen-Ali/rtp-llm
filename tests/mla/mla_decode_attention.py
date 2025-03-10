import logging
import math
import os
import unittest
import torch

from dataclasses import dataclass
from test_util import compare_tensor_diff_with_ratio
from typing import List
import random

os.environ['DEVICE_RESERVE_MEMORY_BYTES'] = '128000000'

logging.basicConfig(
    level="INFO",
    format='%(message)s')

def set_seed(seed: int):
    random.seed(seed)
    torch.manual_seed(seed)
    torch.cuda.manual_seed(seed)
    torch.cuda.manual_seed_all(seed)

os.environ["ENABLE_TRTV1_FMHA"] = "OFF"
os.environ["ENABLE_TRT_FMHA"] = "OFF"

def yarn_get_mscale(scale: float = 1, mscale: float = 1):
    if scale <= 1:
        return 1.0
    return 0.1 * mscale * math.log(scale) + 1.0


@dataclass
class DeepSeekConfig:
    head_num = 128
    nope_head_size = 128
    rope_head_size = 64
    nope_rope_size = 192
    v_head_size = 128
    q_lora_rank = 1536
    kv_lora = 512
    hidden_size = 7168
    mscale = yarn_get_mscale(40, 1.0)
    softmax_scale = 192**-0.5 * mscale * mscale


def attention_ref(
    batch_size: int,
    q: torch.Tensor,
    k: torch.Tensor,
    v: torch.Tensor,
    causal: bool,
    sm_scale: float,
) -> torch.Tensor:
    qo_len = q.shape[0] // batch_size
    kv_len = k.shape[0] // batch_size
    num_qo_heads = q.shape[1]
    head_dim_qk = q.shape[2]
    head_dim_vo = v.shape[2]
    logits = (
        torch.einsum(
            "bmhd,bnhd->bhmn",
            q.view(batch_size, qo_len, num_qo_heads, head_dim_qk).float(),
            k.view(batch_size, kv_len, num_qo_heads, head_dim_qk).float(),
        )
        * sm_scale
    )

    if causal:
        mask = (
            torch.arange(kv_len - qo_len, kv_len).unsqueeze(1)
            >= torch.arange(0, kv_len).unsqueeze(0)
        ).to(q.device)
    else:
        mask = torch.ones(qo_len, kv_len).to(q.device)

    logits = logits.masked_fill(mask.unsqueeze(0).unsqueeze(0) == 0, float("-inf"))
    lse_ref = torch.logsumexp(logits, -1).transpose(-1, -2)
    p = torch.softmax(logits, dim=-1)
    o_ref = (
        torch.einsum(
            "bhmn,bnhd->bmhd",
            p,
            v.view(batch_size, kv_len, num_qo_heads, head_dim_vo).float(),
        )
        .contiguous()
        .view(batch_size * qo_len, num_qo_heads, head_dim_vo)
        .to(q)
    )

    return o_ref, lse_ref * math.log2(math.e)

class TestMlaDecodeAttention(unittest.TestCase):
    def __init__(self, methodName: str = "runTest") -> None:
        super().__init__(methodName)
        torch.classes.load_library(os.environ['TEST_SRCDIR'] + "/maga_transformer/tests/libtest_ops.so")        
        self.config = DeepSeekConfig()
        self.mla_decode_attn_op = torch.classes.unittest.MlaDecoderAttnOp(
            self.config.head_num,
            self.config.nope_head_size,
            self.config.rope_head_size,
            self.config.v_head_size,
            self.config.q_lora_rank,
            self.config.kv_lora,
            self.config.hidden_size,
            self.config.mscale * self.config.mscale,
        )

    def _test_one_case(self, sequence_lengths: List[int], page_size: int):
        batch_size = len(sequence_lengths)
        total_token_num = len(sequence_lengths)
        seq_page_sizes = [math.ceil(x / page_size) for x in sequence_lengths]

        kvcache_block_id = torch.zeros([batch_size, max(seq_page_sizes)], dtype=torch.int32)
        bias = 0
        for i in range(batch_size):
            kvcache_block_id[i, :seq_page_sizes[i]] = torch.arange(bias, bias + seq_page_sizes[i], dtype=torch.int32, device=torch.device("cuda"))
            bias += seq_page_sizes[i]

        q_total = torch.randn([total_token_num, self.config.head_num, self.config.nope_head_size + self.config.rope_head_size], dtype=torch.bfloat16, device=torch.device("cuda"))
        q_nope, q_rope = q_total[:, :, :self.config.nope_head_size], q_total[:, :, self.config.nope_head_size:]
        total_page_num = sum(seq_page_sizes)
        ckv_cache = torch.randn([total_page_num, page_size, self.config.kv_lora], dtype=torch.bfloat16, device=torch.device("cuda"))
        kpe_cache = torch.randn([total_page_num, page_size, self.config.rope_head_size], dtype=torch.bfloat16, device=torch.device("cuda"))
        kc_weight_b = torch.randn([self.config.head_num, self.config.nope_head_size, self.config.kv_lora], dtype=torch.bfloat16, device=torch.device("cuda"))
        vc_t_weight_b = torch.randn([self.config.head_num, self.config.kv_lora, self.config.nope_head_size], dtype=torch.bfloat16, device=torch.device("cuda"))
        sequence_lengths_mius_1 = [x - 1 for x in sequence_lengths]
        sequence_lengths_t = torch.tensor(sequence_lengths_mius_1, dtype=torch.int32)

        ft_qkv_out = self.mla_decode_attn_op.forward(q_total, kc_weight_b, vc_t_weight_b, ckv_cache, kpe_cache, sequence_lengths_t, kvcache_block_id, page_size)

        page_bias = 0
        logging.debug(f"---------------Case {sequence_lengths}, {page_size}---------------")
        for i in range(len(seq_page_sizes)):
            q_absorb = torch.bmm(q_nope[i].unsqueeze(0).transpose(0, 1), kc_weight_b).transpose(0, 1).contiguous()
            q_cat = torch.cat([q_absorb, q_rope[i].unsqueeze(0)], dim=-1)
            ckv = ckv_cache[page_bias:page_bias + seq_page_sizes[i]].view(-1, self.config.kv_lora)[:sequence_lengths[i]]
            kpe = kpe_cache[page_bias:page_bias + seq_page_sizes[i]].view(-1, self.config.rope_head_size)[:sequence_lengths[i]]
            k = torch.cat([ckv, kpe], dim=-1).view(-1, 1, self.config.kv_lora + self.config.rope_head_size).repeat_interleave(self.config.head_num, dim=1)
            v = ckv.view(-1, 1, self.config.kv_lora).repeat_interleave(self.config.head_num, dim=1)
            qkv_out, _ = attention_ref(1, q_cat, k, v, True, self.config.softmax_scale)
            attn_out = torch.bmm(qkv_out.transpose(0, 1), vc_t_weight_b).transpose(0, 1).contiguous()
            compare_tensor_diff_with_ratio(attn_out[0], ft_qkv_out[i], rel_threshold=1e-2, abs_threshold=1e-3, name="Batch " + str(i))
            page_bias += seq_page_sizes[i]

    def _test_prepare_flash_infer_params(self, input_lengths: List[int], sequence_lengths: List[int], page_size: int):
        batch_size = len(input_lengths)
        decode_batch_size = len(sequence_lengths)
    
        sequence_length_minus_1_t = torch.tensor([x - 1 for x in sequence_lengths], dtype=torch.int32)
        input_length_t = torch.tensor(input_lengths, dtype=torch.int32)
        page_nums = [math.ceil(x / page_size) for x in sequence_lengths]  + [math.ceil(x / page_size) for x in input_lengths[decode_batch_size:]]
        block_id_map = torch.zeros([batch_size, max(page_nums)], dtype=torch.int32)
        for i in range(block_id_map.size(0)):
            for j in range(block_id_map.size(1)):
                block_id_map[i, j] = i*block_id_map.size(1) + j
        flash_infer_params = self.mla_decode_attn_op.createFlashInferParams(sequence_length_minus_1_t, input_length_t, page_size, block_id_map)
        expect_batch_indices = []
        expect_positions = []
        expect_kv_last_page_len = []
        expect_page_indptr = [0]
        expect_page_indices = []
        # general
        for i in range(len(input_lengths)):
            expect_page_indptr.append(int(page_nums[i] + expect_page_indptr[-1]))
            expect_page_indices.extend(block_id_map[i, :page_nums[i]])

        # decode 
        for i in range(len(sequence_lengths)):
            expect_batch_indices.append(i)
            expect_positions.append(sequence_lengths[i] - 1)
            expect_kv_last_page_len.append((sequence_lengths[i] - 1) % page_size + 1)
        # prefill
        for i in range(len(sequence_lengths), len(input_lengths)):
            expect_batch_indices.extend([i] * input_lengths[i])
            expect_positions.extend(list(range(input_lengths[i])))
            expect_kv_last_page_len.append((input_lengths[i] - 1) % page_size + 1)
        self.assertEqual(flash_infer_params.batch_indices.tolist(), expect_batch_indices)
        self.assertEqual(flash_infer_params.positions.tolist(), expect_positions)
        self.assertEqual(flash_infer_params.kv_last_page_len.tolist(), expect_kv_last_page_len)
        self.assertEqual(flash_infer_params.page_indptr.tolist(), expect_page_indptr)
        self.assertEqual(flash_infer_params.page_indices.tolist(), expect_page_indices)
    
    def test_prepare_flash_infer_params(self):
        self._test_prepare_flash_infer_params([25, 82], [26], 64)
        self._test_prepare_flash_infer_params([25], [26], 64)
        self._test_prepare_flash_infer_params([25], [26], 16)
        self._test_prepare_flash_infer_params([25, 998], [26, 999], 16)
        self._test_prepare_flash_infer_params([25, 998], [26], 64)
        self._test_prepare_flash_infer_params([25, 998], [26], 64)
        self._test_prepare_flash_infer_params([25, 998, 1084], [26], 64)
        self._test_prepare_flash_infer_params([25, 998, 1084], [26, 1002], 64)

    def test_mla_decode(self):
        set_seed(42)
        self._test_one_case([25], 64)
        self._test_one_case([25], 64)
        self._test_one_case([99, 65], 64)
        self._test_one_case([17, 25], 64)
        self._test_one_case([25], 16)
        self._test_one_case([25], 16)
        self._test_one_case([99, 65], 16)
        self._test_one_case([17, 25], 16)
        self._test_one_case([1024], 64)
        self._test_one_case([1025], 64)
        self._test_one_case([129], 64)
        self._test_one_case([128], 64)
        self._test_one_case([127], 64)


if __name__ == '__main__':
    unittest.main()
