#!/bin/sh
n=0
while IFS= read -r line; do
  case "$line" in
    *'"type":"user"'*)
      n=$((n+1))
      # Qucs-S's own tools (qucscontrol.h), now and then with arguments no
      # one would give: called as claude calls them, their answers ignored.
      case $((n % 8)) in
        0) printf '%s\n' '{"type":"control_request","request_id":"q'$n'a","request":{"subtype":"mcp_message","server_name":"qucs","message":{"method":"tools/call","params":{"name":"add_component","arguments":{"type":"R","x":'$((n * 10))',"y":100,"properties":{"R":"2147483647"}}},"jsonrpc":"2.0","id":'$((n * 10))'}}}'
           printf '%s\n' '{"type":"control_request","request_id":"q'$n'b","request":{"subtype":"mcp_message","server_name":"qucs","message":{"method":"tools/call","params":{"name":"connect","arguments":{"from":"R1.1","to":[0,0]}},"jsonrpc":"2.0","id":'$((n * 10 + 1))'}}}' ;;
        1) printf '%s\n' '{"type":"control_request","request_id":"q'$n'a","request":{"subtype":"mcp_message","server_name":"qucs","message":{"method":"tools/call","params":{"name":"edit_component","arguments":{"name":"R1","rotation":7,"x":99999999,"y":-2147483648,"properties":{"R":"1e308k"},"rename":"","active":false}},"jsonrpc":"2.0","id":'$((n * 10))'}}}'
           printf '%s\n' '{"type":"control_request","request_id":"q'$n'b","request":{"subtype":"mcp_message","server_name":"qucs","message":{"method":"tools/call","params":{"name":"screenshot","arguments":{"area":"visible"}},"jsonrpc":"2.0","id":'$((n * 10 + 1))'}}}' ;;
        2) printf '%s\n' '{"type":"control_request","request_id":"q'$n'a","request":{"subtype":"mcp_message","server_name":"qucs","message":{"method":"tools/call","params":{"name":"add_wire","arguments":{"points":[[0,0],[2147483647,-2147483648],[5,5]]}},"jsonrpc":"2.0","id":'$((n * 10))'}}}'
           printf '%s\n' '{"type":"control_request","request_id":"q'$n'b","request":{"subtype":"mcp_message","server_name":"qucs","message":{"method":"tools/call","params":{"name":"set_label","arguments":{"at":[0,0],"name":"net'$n'"}},"jsonrpc":"2.0","id":'$((n * 10 + 1))'}}}' ;;
        3) printf '%s\n' '{"type":"control_request","request_id":"q'$n'a","request":{"subtype":"mcp_message","server_name":"qucs","message":{"method":"tools/call","params":{"name":"get_schematic","arguments":{"format":"text"}},"jsonrpc":"2.0","id":'$((n * 10))'}}}'
           printf '%s\n' '{"type":"control_request","request_id":"q'$n'b","request":{"subtype":"mcp_message","server_name":"qucs","message":{"method":"tools/call","params":{"name":"set_schematic","arguments":{"text":"<Components>\n  <R R9 1 0 0 15 -26 0 0 \"1\" 1>\n  <Nope X 1>\n</Components>"}},"jsonrpc":"2.0","id":'$((n * 10 + 1))'}}}' ;;
        4) printf '%s\n' '{"type":"control_request","request_id":"q'$n'a","request":{"subtype":"mcp_message","server_name":"qucs","message":{"method":"tools/call","params":{"name":"delete","arguments":{"names":["R1","nobody"],"wires":[[0,0,0,0],[1]]}},"jsonrpc":"2.0","id":'$((n * 10))'}}}'
           printf '%s\n' '{"type":"control_request","request_id":"q'$n'b","request":{"subtype":"mcp_message","server_name":"qucs","message":{"method":"tools/call","params":{"name":"undo","arguments":{}},"jsonrpc":"2.0","id":'$((n * 10 + 1))'}}}' ;;
        5) printf '%s\n' '{"type":"control_request","request_id":"q'$n'a","request":{"subtype":"mcp_message","server_name":"qucs","message":{"method":"tools/call","params":{"name":"trigger_action","arguments":{"action":"Edit > Select All"}},"jsonrpc":"2.0","id":'$((n * 10))'}}}'
           printf '%s\n' '{"type":"control_request","request_id":"q'$n'b","request":{"subtype":"mcp_message","server_name":"qucs","message":{"method":"tools/call","params":{"name":"get_dialog","arguments":{}},"jsonrpc":"2.0","id":'$((n * 10 + 1))'}}}' ;;
        6) printf '%s\n' '{"type":"control_request","request_id":"q'$n'a","request":{"subtype":"mcp_message","server_name":"qucs","message":{"method":"tools/call","params":{"name":"set_schematic","arguments":{"text":"<Wires>\n  <0 0 100 0 \"\" 0 0 0 \"\">\n</Wires>"}},"jsonrpc":"2.0","id":'$((n * 10))'}}}'
           printf '%s\n' '{"type":"control_request","request_id":"q'$n'b","request":{"subtype":"mcp_message","server_name":"qucs","message":{"method":"tools/call","params":{"name":"zoom","arguments":{"to":"out"}},"jsonrpc":"2.0","id":'$((n * 10 + 1))'}}}' ;;
        *) printf '%s\n' '{"type":"control_request","request_id":"q'$n'a","request":{"subtype":"mcp_message","server_name":"qucs","message":{"method":"tools/call","params":{"name":"get_state","arguments":{}},"jsonrpc":"2.0","id":'$((n * 10))'}}}'
           printf '%s\n' '{"type":"control_request","request_id":"q'$n'b","request":{"subtype":"mcp_message","server_name":"qucs","message":{"method":"tools/call","params":{"name":"select","arguments":{"names":["R1",7,null]}},"jsonrpc":"2.0","id":'$((n * 10 + 1))'}}}' ;;
      esac
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
    *'"mcp_response"'*)
      ;;
    *'"type":"control_response"'*|*'"subtype":"interrupt"'*)
      echo '{"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"t'$n'","content":"done","is_error":false}]}}'
      echo '{"type":"result","subtype":"success","is_error":false,"duration_ms":10,"num_turns":1,"result":"ok","total_cost_usd":0.001}'
      ;;
  esac
done
