# llama.qtcreator

Local LLM-assisted text completion for Qt Creator.

![Qt Creator llama.cpp Text](https://raw.githubusercontent.com/cristianadam/llama.qtcreator/refs/heads/main/screenshots/qtcreator-llamacpp-text@2x.webp)

---

![Qt Creator llama.cpp Qt Widgets](https://raw.githubusercontent.com/cristianadam/llama.qtcreator/refs/heads/main/screenshots/qtcreator-llamacpp-widgets@2x.webp)


## Features

- Auto-suggest on cursor movement. Toggle enable / disable with `Ctrl+Shift+G`
- Trigger the suggestion manually by pressing `Ctrl+G`
- Accept a suggestion with `Tab`
- Accept the first line of a suggestion with `Shift+Tab`
- Control max text generation time
- Configure scope of context around the cursor
- Ring context with chunks from open and edited files and yanked text
- [Supports very large contexts even on low-end hardware via smart context reuse](https://github.com/ggml-org/llama.cpp/pull/9787)
- Speculative FIM support
- Speculative Decoding support
- Display performance stats
- Chat support
- Source and Image drag & drop support
- Current editor selection predefined and custom LLM prompts
- [Tools usage](https://github.com/cristianadam/llama.qtcreator/wiki/Tools)

### llama.cpp setup

The plugin requires a [llama.cpp](https://github.com/ggml-org/llama.cpp) server instance to be running at:

![Qt Creator llama.cpp Settings](https://raw.githubusercontent.com/cristianadam/llama.qtcreator/refs/heads/main/screenshots/qtcreator-llamacpp-settings@2x.webp)


#### Mac OS

```bash
brew install llama.cpp
```

#### Windows

```bash
winget install llama.cpp
```

#### Any other OS

Either build from source or use the latest binaries: https://github.com/ggml-org/llama.cpp/releases

### llama.cpp settings

Here are recommended settings, depending on the amount of VRAM that you have:

- More than 16GB VRAM:

  ```bash
  llama-server --fim-qwen-7b-default
  ```

- Less than 16GB VRAM:

  ```bash
  llama-server --fim-qwen-3b-default
  ```

- Less than 8GB VRAM:

  ```bash
  llama-server --fim-qwen-1.5b-default
  ```

Use `llama-server --help` for more details.


### Recommended LLMs

The plugin requires FIM-compatible models: [HF collection](https://huggingface.co/collections/ggml-org/llamavim-6720fece33898ac10544ecf9)

## Examples

### A Qt Quick example on MacBook Pro M3 `Qwen2.5-Coder 3B Q8_0`:

![Qt Creator llama.cpp Qt Quick](https://raw.githubusercontent.com/cristianadam/llama.qtcreator/refs/heads/main/screenshots/qtcreator-llamacpp-quick@2x.webp)

### Chat on a Mac Studio M2 with `gpt-oss 20B`:

![Qt Creator llama.cpp Chat](https://raw.githubusercontent.com/cristianadam/llama.qtcreator/refs/heads/main/screenshots/qtcreator-llamacpp-chat.webp)

### Using Tools on a MacBook M3 with `gpt-oss 20B`:

https://github.com/user-attachments/assets/b38f2d6b-aea8-4879-be17-73f0290e4f71

## Implementation details

The plugin aims to be very simple and lightweight and at the same time to provide high-quality and performant local FIM completions, even on consumer-grade hardware. 

## Other IDEs

- Vim/Neovim: https://github.com/ggml-org/llama.vim
- VS Code: https://github.com/ggml-org/llama.vscode
