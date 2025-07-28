import json
import logging
import os
import re
import shutil
import threading
from enum import Enum
from pathlib import Path
from typing import Any, Dict, List, Optional, Set, Union

import aiohttp
import pynvml
import requests
import torch
from aiohttp import ClientConnectorError, ClientTimeout, ServerTimeoutError

from rtp_llm import _ft_pickler


class AtomicCounter:
    def __init__(self, initial: int = 0):
        self.initial = initial
        self.value = initial
        self._lock = threading.Lock()

    def increment(self):
        with self._lock:
            self.value += 1
            return self.value

    def decrement(self):
        with self._lock:
            self.value -= 1
            return self.value

    def decrement_if_gt_0(self):
        with self._lock:
            if self.value > 0:
                self.value -= 1
                return True
            return False

    def get(self):
        with self._lock:
            return self.value

    def reset(self):
        with self._lock:
            self.value = self.initial


PathLike = Union[str, Path]


def to_torch_dtype(maybe_str_dtype: Union[str, torch.dtype]) -> torch.dtype:
    if isinstance(maybe_str_dtype, torch.dtype):
        dtype = maybe_str_dtype
    else:
        try:
            dtype = {
                "bf16": torch.bfloat16,
                "fp16": torch.float16,
                "fp32": torch.float32,
                "bfloat16": torch.bfloat16,
                "float16": torch.float16,
                "float32": torch.float32,
                "int8": torch.int8,
            }[maybe_str_dtype.lower()]
        except KeyError:
            raise ValueError(
                f"Cannot convert to torch data type, got {maybe_str_dtype}"
            )
    return dtype


def check_get_config_from_path(ckpt_path: str) -> Dict[str, Any]:
    config_json = get_config_from_path(ckpt_path)
    if config_json is None:
        raise Exception(f"Failed to get config.json from path: {ckpt_path}")
    return config_json


def get_config_from_path(ckpt_path: str) -> Optional[Dict[str, Any]]:
    if os.path.isdir(ckpt_path):
        # load from huggingface
        config_json_path = os.path.join(ckpt_path, "config.json")
        if os.path.isfile(config_json_path):
            with open(config_json_path, "r", encoding="utf-8") as reader:
                text = reader.read()
                config_dict = json.loads(text)
                return config_dict
    return None


def generate_pad_mask(
    input_lengths: torch.Tensor, memory_length: int, init_step: int = 0
):
    """Generate a pad mask tensor.

    # Args.
        input_lengths: (batch_size * beam_width,), input lengths
        memory_length: the length of key/value cache memory.
        init_step: int, initial step.
    # Return
        masked_tokens: BoolTensor, (batch_size * beam_width, memory_length),
            True if init_step + input_length[i] <= j < init_step + max_input_length,
            where i is a batch-beam index and j is a time step modulo by memory_length.
    """
    max_input_length = input_lengths.max()
    input_lengths = input_lengths.unsqueeze(1)
    shift = init_step % memory_length
    step_indices = torch.arange(
        init_step, init_step + memory_length, device=input_lengths.device
    )
    step_indices = step_indices.roll(shift).unsqueeze(0).tile(input_lengths.shape[0], 1)
    masked_tokens = torch.logical_and(
        step_indices >= input_lengths, step_indices < init_step + max_input_length
    )
    return masked_tokens


def get_ckpt_file_from_index(ckpt_path: str, model_index_file: str) -> List[str]:
    with open(model_index_file) as reader:
        index_json = json.loads(reader.read())
    ckpt_set: Set[str] = set()
    for _, ckpt_file in index_json["weight_map"].items():
        ckpt_set.add(ckpt_file)
    return [os.path.join(ckpt_path, ckpt_file) for ckpt_file in ckpt_set]


def load_ckpt(ckpt_path: str) -> Dict[str, Any]:
    if os.path.isfile(ckpt_path):
        return torch.load(ckpt_path, map_location="cpu", pickle_module=_ft_pickler)
    elif os.path.isdir(ckpt_path):
        # just support from huggingface
        model_index_file = os.path.join(ckpt_path, "pytorch_model.bin.index.json")
        if os.path.exists(model_index_file):
            checkpoints = get_ckpt_file_from_index(ckpt_path, model_index_file)
        else:
            checkpoints = sorted(Path(ckpt_path).glob("*.bin"))
        params: Dict[str, torch.Tensor] = {}
        for ckpt in checkpoints:
            params.update(
                torch.load(ckpt, map_location="cpu", pickle_module=_ft_pickler)
            )
        return params
    else:
        raise NotImplementedError(
            f"just support pt file or huggingface: ckpt_path:{ckpt_path}"
        )


def copy_gemm_config():
    if "HIPPO_APP_INST_ROOT" in os.environ:
        inst_root = os.environ["HIPPO_APP_INST_ROOT"]
        gemm_config_path = os.path.join(inst_root, "gemm_config.in")
        if os.path.exists(gemm_config_path):
            logging.info("Found gemm_config, copy to current path")
            shutil.copy(gemm_config_path, ".")
            return
    logging.info("not found gemm_config in HIPPO_APP_INST_ROOT, not copy")


def get_dtype_size(dtype: torch.dtype) -> int:
    return {torch.int8: 1, torch.half: 2, torch.bfloat16: 2, torch.float: 4}[dtype]


def check_with_info(condition: bool, error_msg: str):
    if not condition:
        raise Exception(error_msg)


def str_to_bool(s: str):
    true_values = ("yes", "true", "1")
    false_values = ("no", "false", "0")
    if s.lower() in true_values:
        return True
    elif s.lower() in false_values:
        return False
    else:
        raise ValueError("Cannot covert {} to a bool".format(s))


def closest_power_of_2(x):
    if x < 1:
        return 1
    power = 1
    while power * 2 <= x:
        power *= 2
    return power


# a's suffix is equal to b's prefix
def has_overlap(a: str, b: str) -> bool:
    max_possible = min(len(a), len(b))
    for k in range(1, max_possible + 1):
        if a[-k:] == b[:k]:
            return True
    return False


# a's suffix is equal to b's prefix
def has_overlap_kmp(a: str, b: str) -> bool:
    if len(a) > len(b):
        a = a[-(len(b) + 1) :]
    s = b + "#" + a
    prefix = [0] * len(s)
    for i in range(1, len(s)):
        j = prefix[i - 1]
        while j > 0 and s[i] != s[j]:
            j = prefix[j - 1]
        if s[i] == s[j]:
            j += 1
        prefix[i] = j
    return prefix[-1] > 0


async def async_request_server(
    method: str, server_port: int, uri: str = "", req: Dict[str, Any] = None
) -> Dict[str, Any]:
    """
    异步HTTP请求服务 (基于aiohttp实现)

    :param method: 请求方法，支持 GET/POST（不区分大小写）
    :param server_port: 服务端口号
    :param uri: 请求路径（自动处理首尾斜杠）
    :param req: 请求参数（GET=URL参数，POST=JSON body）
    :return: 包含响应数据或错误信息的字典
    """
    req = req or {}
    url = f"http://localhost:{server_port}/{uri.strip('/')}"
    # timeout = aiohttp.ClientTimeout(total=50, connect=30)  # 总超时50s，连接超时30s

    try:
        async with aiohttp.ClientSession() as session:
            # 统一转换为小写方法名
            method = method.lower()

            if method == "get":
                async with session.get(url, params=req) as response:
                    return await _handle_response(response)
            elif method == "post":
                async with session.post(url, json=req) as response:
                    return await _handle_response(response)
            else:
                return {"error": f"Unsupported HTTP method: {method}"}
    # 明确区分错误类型
    except ClientConnectorError as e:
        # 连接失败（如DNS解析错误、TCP连接拒绝）
        return {"error": "Connection failed", "details": str(e)}
    except ServerTimeoutError as e:
        # 超时错误（连接或响应超时）
        return {"error": "Request timeout", "details": str(e)}
    except aiohttp.ClientError as e:
        # 其他客户端错误（如无效URL、HTTP协议错误）
        return {"error": "Client error", "details": str(e)}
    except Exception as e:
        # 未知错误
        return {"error": "Unexpected error", "details": str(e)}


async def _handle_response(response: aiohttp.ClientResponse) -> Dict[str, Any]:
    """统一处理HTTP响应"""
    try:
        # 处理非200状态码
        if response.status != 200:
            error_text = await response.text()
            return {
                "error": f"HTTP Error {response.status}",
                "details": error_text,
            }

        # 尝试解析JSON
        return await response.json()

    except (aiohttp.ContentTypeError, ValueError) as e:
        # JSON解析失败时返回原始文本
        text = await response.text()
        return {
            "error": "Invalid JSON response",
            "details": str(e),
            "raw_response": text,
        }
