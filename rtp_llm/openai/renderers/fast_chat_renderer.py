from transformers import PreTrainedTokenizerBase

from rtp_llm.openai.api_datatype import ChatCompletionRequest, RendererInfo, RoleEnum
from rtp_llm.openai.renderers.custom_renderer import (
    CustomChatRenderer,
    RenderedInputs,
    RendererParams,
)

from .conversation import get_conv_template


class FastChatRenderer(CustomChatRenderer):
    def __init__(
        self, tokenizer: PreTrainedTokenizerBase, renderer_params: RendererParams
    ):
        super().__init__(tokenizer, renderer_params)
        self.conv_template = get_conv_template(renderer_params.model_type)
        self.roles_map = {
            RoleEnum.user: self.conv_template.roles[0],
            RoleEnum.assistant: self.conv_template.roles[1],
        }

        if isinstance(self.conv_template.stop_str, list):
            self.add_extra_stop_words(self.conv_template.stop_str)
        elif isinstance(self.conv_template.stop_str, str):
            self.add_extra_stop_words([self.conv_template.stop_str])
        if self.conv_template.stop_token_ids:
            self.add_extra_stop_word_ids(
                [[id] for id in self.conv_template.stop_token_ids]
            )

    def get_renderer_info(self) -> RendererInfo:
        renderer_info = super().get_renderer_info()
        renderer_info.template = str(self.conv_template)
        return renderer_info

    def render_chat(self, request: ChatCompletionRequest) -> RenderedInputs:
        conversaion = self.conv_template.copy()

        for message in request.messages:
            assert isinstance(message.content, str)
            if message.role == RoleEnum.system:
                conversaion.set_system_message(message.content)
            else:
                conversaion.append_message(
                    self.roles_map[message.role], message.content
                )
        if request.messages[-1].role != RoleEnum.assistant:
            conversaion.append_message(self.roles_map[RoleEnum.assistant], "")

        prompt = conversaion.get_prompt()
        input_ids = self.tokenizer.encode(prompt)

        return RenderedInputs(input_ids=input_ids)
