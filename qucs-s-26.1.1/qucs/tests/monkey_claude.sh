#!/bin/sh
n=0
while IFS= read -r line; do
  case "$line" in
    *'"type":"user"'*)
      n=$((n+1))
      echo '{"type":"system","subtype":"init","session_id":"monkey","model":"claude-monkey-1"}'
      echo '{"type":"stream_event","event":{"type":"content_block_start","content_block":{"type":"text","text":""}}}'
      echo '{"type":"stream_event","event":{"type":"content_block_delta","delta":{"type":"text_delta","text":"Looking **at** it"}}}'
      echo 'a line that is not the protocol'
      echo '{"type":"assistant","message":{"content":[{"type":"text","text":"# A heading

- one
- two

```
code
```

[a link](https://example.org) to follow"}]}}'
      echo '{"type":"assistant","message":{"content":[{"type":"tool_use","id":"t'$n'","name":"Edit","input":{"file_path":"'"$QUCS_FAKE_EDIT"'","old_string":"a","new_string":"a"}}]}}'
      echo '{"type":"control_request","request_id":"r'$n'","request":{"subtype":"can_use_tool","tool_name":"Edit","input":{"file_path":"'"$QUCS_FAKE_EDIT"'","old_string":"a","new_string":"a"},"permission_suggestions":[{"type":"setMode","mode":"acceptEdits","destination":"session"}]}}'
      if [ $((n % 5)) -eq 4 ]; then echo 'dying' >&2; exit 7; fi
      ;;
    *'"type":"control_response"'*|*'"subtype":"interrupt"'*)
      echo '{"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"t'$n'","content":"done","is_error":false}]}}'
      echo '{"type":"result","subtype":"success","is_error":false,"duration_ms":10,"num_turns":1,"result":"ok","total_cost_usd":0.001}'
      ;;
  esac
done
