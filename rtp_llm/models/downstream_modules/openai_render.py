import logging
import os
from transformers import PreTrainedTokenizerBase
from rtp_llm.config.gpt_init_model_parameters import GptInitModelParameters
from rtp_llm.openai.renderers.custom_renderer import RendererParams, CustomChatRenderer
from rtp_llm.openai.renderer_factory import ChatRendererFactory
from rtp_llm.openai.renderers.basic_renderer import BasicRenderer



class OpenAIRenderBasicInfo(object):
    def __init__(self, tokenizer: PreTrainedTokenizerBase, config: GptInitModelParameters):
        self.config = config
        self.max_seq_len = self.config.max_seq_len

        if tokenizer is None:
            raise AttributeError(f"model has no tokenizer!")
        self.tokenizer = tokenizer

        self.eos_token_id = None
        if (isinstance(tokenizer, PreTrainedTokenizerBase)):
            self.eos_token_id = tokenizer.eos_token_id
        if self.eos_token_id is None:
            self.eos_token_id = self.config.special_tokens.eos_token_id

        self.stop_word_ids_list = self.config.special_tokens.stop_words_id_list

        render_params = RendererParams(
            model_type=config.py_env_configs.model_config.model_type,
            max_seq_len=self.max_seq_len,
            eos_token_id=self.eos_token_id,
            stop_word_ids_list=self.stop_word_ids_list,
            template_type=self.config.template_type,
            ckpt_path=self.config.ckpt_path
        )

        self.chat_renderer: CustomChatRenderer = ChatRendererFactory.get_renderer(self.tokenizer, render_params)
        logging.info(f"Finally openai endpoint uses renderer: {self.chat_renderer} ")
        self.template_renderer: CustomChatRenderer = self.chat_renderer \
            if isinstance(self.chat_renderer,BasicRenderer) \
            else BasicRenderer(self.tokenizer, render_params)
        logging.info(f"chat_renderer [{self.chat_renderer}] is created.")
        extra_stop_word_ids_list = self.chat_renderer.get_all_extra_stop_word_ids_list()
        self.stop_word_ids_list.extend(extra_stop_word_ids_list)
        self.stop_words_list = []
        for stop_word_ids in self.stop_word_ids_list:
            word = self.tokenizer.decode(stop_word_ids)
            if len(word):
                self.stop_words_list.append(word)
        logging.info(f"use stop_words_list [{self.stop_words_list}]")
