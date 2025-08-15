<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE TS>
<TS version="2.1" language="zh_CN">
  <context>
    <name>LlamaCpp</name>
    <message>
      <location filename="llamaplugin.cpp" line="+70"/>
      <source>Request llama.cpp Suggestion</source>
      <translation>请求 llama.cpp 建议</translation>
    </message>
    <message>
      <location line="+2"/>
      <source>Request llama.cpp suggestion at the current editor's cursor position.</source>
      <translation>在当前编辑器的光标位置请求 llama.cpp 建议。</translation>
    </message>
    <message>
      <location line="+9"/>
      <source>Ctrl+G</source>
      <translation>Ctrl+G</translation>
    </message>
    <message>
      <location line="+3"/>
      <source>Toggle enable/disable llama.cpp</source>
      <translation>切换启用/禁用 llama.cpp</translation>
    </message>
    <message>
      <location line="+13"/>
      <source>Disable llama.cpp.</source>
      <translation>禁用 llama.cpp。</translation>
    </message>
    <message>
      <location line="+0"/>
      <source>Enable llama.cpp.</source>
      <translation>启用 llama.cpp。</translation>
    </message>
    <message>
      <location line="+349"/>
      <source>[llama.cpp] Error fetching fim completion from %1: %2</source>
      <translation>[llama.cpp] 从 %1 获取 fim 补全时出错：%2</translation>
    </message>
    <message>
      <location filename="llamaprojectpanel.cpp" line="+63"/>
      <source>llama.cpp</source>
      <translation>llama.cpp</translation>
    </message>
    <message>
      <location filename="llamasettings.cpp" line="+18"/>
      <location line="+1"/>
      <source>Enable llama.cpp</source>
      <translation>启用 llama.cpp</translation>
    </message>
    <message>
      <location line="+1"/>
      <source>Enables the llama.cpp integration.</source>
      <translation>启用 llama.cpp 集成。</translation>
    </message>
    <message>
      <location line="+14"/>
      <source>Endpoint</source>
      <translation>端点</translation>
    </message>
    <message>
      <location line="+3"/>
      <source>Endpoint:</source>
      <translation>端点：</translation>
    </message>
    <message>
      <location line="+2"/>
      <source>llama.cpp server endpoint</source>
      <translation>llama.cpp 服务器端点</translation>
    </message>
    <message>
      <location line="+3"/>
      <source>API Key</source>
      <translation>API 密钥</translation>
    </message>
    <message>
      <location line="+3"/>
      <source>API Key:</source>
      <translation>API 密钥：</translation>
    </message>
    <message>
      <location line="+2"/>
      <source>llama.cpp server api key (optional)</source>
      <translation>llama.cpp 服务器 API 密钥（可选）</translation>
    </message>
    <message>
      <location line="+3"/>
      <source>Prefix Code Lines</source>
      <translation>前缀代码行</translation>
    </message>
    <message>
      <location line="+2"/>
      <source>Prefix Code Lines:</source>
      <translation>前缀代码行：</translation>
    </message>
    <message>
      <location line="+3"/>
      <source>Number of code lines before the cursor location to include in the local prefix.</source>
      <translation>在光标位置之前要包含在本地前缀中的代码行数。</translation>
    </message>
    <message>
      <location line="+3"/>
      <source>Suffix Code Lines</source>
      <translation>后缀代码行</translation>
    </message>
    <message>
      <location line="+2"/>
      <source>Suffix Code Lines:</source>
      <translation>后缀代码行：</translation>
    </message>
    <message>
      <location line="+3"/>
      <source>Number of code lines after  the cursor location to include in the local suffix.</source>
      <translation>在光标位置之后要包含在本地后缀中的代码行数。</translation>
    </message>
    <message>
      <location line="+3"/>
      <source>Max Token Predictions</source>
      <translation>最大令牌预测数</translation>
    </message>
    <message>
      <location line="+2"/>
      <source>Max Token Predictions:</source>
      <translation>最大令牌预测数：</translation>
    </message>
    <message>
      <location line="+2"/>
      <source>Max number of tokens to predict.</source>
      <translation>要预测的最大令牌数。</translation>
    </message>
    <message>
      <location line="+3"/>
      <source>Stop Strings</source>
      <translation>停止字符串</translation>
    </message>
    <message>
      <location line="+3"/>
      <source>Stop Strings:</source>
      <translation>停止字符串：</translation>
    </message>
    <message>
      <location line="+2"/>
      <source>Return the result immediately as soon as any of these strings are encountered in the generated text. Separated by semicolons.</source>
      <translation>在生成的文本中遇到这些字符串中的任何一个时立即返回结果。用分号分隔。</translation>
    </message>
    <message>
      <location line="+4"/>
      <source>Max Prompt Time (ms)</source>
      <translation>最大提示时间（毫秒）</translation>
    </message>
    <message>
      <location line="+2"/>
      <source>Max Prompt Time (ms):</source>
      <translation>最大提示时间（毫秒）：</translation>
    </message>
    <message>
      <location line="+3"/>
      <source>Max alloted time for the prompt processing (TODO: not yet supported).</source>
      <translation>提示处理的最大分配时间（TODO：尚未支持）。</translation>
    </message>
    <message>
      <location line="+3"/>
      <source>Max Predict Time (ms)</source>
      <translation>最大预测时间（毫秒）</translation>
    </message>
    <message>
      <location line="+2"/>
      <source>Max Predict Time (ms):</source>
      <translation>最大预测时间（毫秒）：</translation>
    </message>
    <message>
      <location line="+2"/>
      <source>Max alloted time for the prediction.</source>
      <translation>预测的最大分配时间。</translation>
    </message>
    <message>
      <location line="+3"/>
      <source>Show Info</source>
      <translation>显示信息</translation>
    </message>
    <message>
      <location line="+2"/>
      <source>Show Info:</source>
      <translation>显示信息：</translation>
    </message>
    <message>
      <location line="+3"/>
      <source>Show extra info about the inference (0 - disabled, 1 - statusline, 2 - inline).</source>
      <translation>显示关于推理的额外信息（0 - 禁用，1 - 状态行，2 - 内联）。</translation>
    </message>
    <message>
      <location line="+3"/>
      <location line="+2"/>
      <source>Auto FIM</source>
      <translation>自动 FIM</translation>
    </message>
    <message>
      <location line="+2"/>
      <source>Trigger FIM (Fill-in-the-Middle) completion automatically on cursor movement.</source>
      <translation>在光标移动时自动触发 FIM（填充中间）补全。</translation>
    </message>
    <message>
      <location line="+2"/>
      <source>Max Line Suffix</source>
      <translation>最大行后缀</translation>
    </message>
    <message>
      <location line="+2"/>
      <source>Max Line Suffix:</source>
      <translation>最大行后缀：</translation>
    </message>
    <message>
      <location line="+2"/>
      <source>Do not auto-trigger FIM completion if there are more than this number of characters to the right of the cursor.</source>
      <translation>如果光标右侧有超过此数量的字符，则不自动触发 FIM 补全。</translation>
    </message>
    <message>
      <location line="+4"/>
      <source>Max Cache Keys</source>
      <translation>最大缓存键</translation>
    </message>
    <message>
      <location line="+2"/>
      <source>Max Cache Keys:</source>
      <translation>最大缓存键：</translation>
    </message>
    <message>
      <location line="+2"/>
      <source>Max number of cached completions to keep in result_cache.</source>
      <translation>在 result_cache 中保留的最大缓存补全数量。</translation>
    </message>
    <message>
      <location line="+3"/>
      <source>Ring Chunks</source>
      <translation>环形块</translation>
    </message>
    <message>
      <location line="+2"/>
      <source>Ring Chunks:</source>
      <translation>环形块：</translation>
    </message>
    <message>
      <location line="+3"/>
      <source>Max number of chunks to pass as extra context to the server (0 to disable).</source>
      <translation>要传递给服务器的额外上下文的最大块数（0 表示禁用）。</translation>
    </message>
    <message>
      <location line="+3"/>
      <source>Chunk Line Size</source>
      <translation>块行大小</translation>
    </message>
    <message>
      <location line="+2"/>
      <source>Chunk Line Size:</source>
      <translation>块行大小：</translation>
    </message>
    <message>
      <location line="+2"/>
      <source>Max size of the chunks (in number of lines).&lt;br/&gt;&lt;br/&gt;Note: adjust these numbers so that you don't overrun your context. At ring_n_chunks = 64 and ring_chunk_size = 64 you need ~32k context.</source>
      <translation>块的最大大小（以行数计）。&lt;br/&gt;&lt;br/&gt;注意：调整这些数字以避免超出上下文。在 ring_n_chunks = 64 和 ring_chunk_size = 64 时，您需要 ~32k 上下文。</translation>
    </message>
    <message>
      <location line="+6"/>
      <source>Ring Line Scope</source>
      <translation>环形行范围</translation>
    </message>
    <message>
      <location line="+2"/>
      <source>Ring Line Scope:</source>
      <translation>环形行范围：</translation>
    </message>
    <message>
      <location line="+2"/>
      <source>The range around the cursor position (in number of lines) for gathering chunks after FIM.</source>
      <translation>在 FIM 之后收集块的范围（以行数计，在光标位置周围）。</translation>
    </message>
    <message>
      <location line="+4"/>
      <source>Update Interval (ms)</source>
      <translation>更新间隔（毫秒）</translation>
    </message>
    <message>
      <location line="+2"/>
      <source>Update Interval (ms):</source>
      <translation>更新间隔（毫秒）：</translation>
    </message>
    <message>
      <location line="+2"/>
      <source>How often to process queued chunks in normal mode.</source>
      <translation>在普通模式下多长时间处理一次排队的块。</translation>
    </message>
  </context>
</TS>
