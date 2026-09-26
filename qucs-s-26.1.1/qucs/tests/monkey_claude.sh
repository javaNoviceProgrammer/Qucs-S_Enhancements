#!/bin/sh
n=0
while IFS= read -r line; do
  case "$line" in
    *'"type":"user"'*)
      n=$((n+1))
      # Qucs-S's own tools (qucscontrol.h), now and then with arguments no
      # one would give: called as claude calls them, their answers ignored.
      # (Counted in a file beside the one it edits: one dies every fifth
      # prompt, and its own count would start again.)
      k=$(cat "$QUCS_FAKE_EDIT.count" 2>/dev/null || echo 0)
      k=$((k + 1))
      echo "$k" > "$QUCS_FAKE_EDIT.count"
      case $((k % 20)) in
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
        8) printf '%s\n' '{"type":"control_request","request_id":"q'$n'a","request":{"subtype":"mcp_message","server_name":"qucs","message":{"method":"tools/call","params":{"name":"add_component","arguments":{"type":"R","name":"Rt","x":'$((n * 30 % 600))',"y":300}},"jsonrpc":"2.0","id":'$((n * 10))'}}}'
           printf '%s\n' '{"type":"control_request","request_id":"q'$n'b","request":{"subtype":"mcp_message","server_name":"qucs","message":{"method":"tools/call","params":{"name":"edit_component","arguments":{"name":"Rt","rotation":'$((n % 4))',"mirror":'$([ $((n % 2)) -eq 0 ] && echo true || echo false)',"x":'$((n * 40 % 700))'}},"jsonrpc":"2.0","id":'$((n * 10 + 1))'}}}' ;;
        9) printf '%s\n' '{"type":"control_request","request_id":"q'$n'a","request":{"subtype":"mcp_message","server_name":"qucs","message":{"method":"tools/call","params":{"name":"connect","arguments":{"from":"Rt.2","to":"R1.1"}},"jsonrpc":"2.0","id":'$((n * 10))'}}}'
           printf '%s\n' '{"type":"control_request","request_id":"q'$n'b","request":{"subtype":"mcp_message","server_name":"qucs","message":{"method":"tools/call","params":{"name":"edit_component","arguments":{"name":"R1","rotation":'$((n % 4))',"mirror":true,"y":'$((n * 20 % 500))'}},"jsonrpc":"2.0","id":'$((n * 10 + 1))'}}}' ;;
        10) printf '%s\n' '{"type":"control_request","request_id":"q'$n'a","request":{"subtype":"mcp_message","server_name":"qucs","message":{"method":"tools/call","params":{"name":"add_diagram","arguments":{"type":"rect","x":'$((n * 10))',"y":400,"width":-5,"height":2147483647,"traces":["v(nowhere)",{"variable":"out","color":"#zzz"},7,null]}},"jsonrpc":"2.0","id":'$((n * 10))'}}}'
           printf '%s\n' '{"type":"control_request","request_id":"q'$n'b","request":{"subtype":"mcp_message","server_name":"qucs","message":{"method":"tools/call","params":{"name":"edit_diagram","arguments":{"diagram":1,"x_axis":{"from":1e308,"to":-1e308,"step":0,"log":true},"y_axis":{"from":0,"to":1e-300},"legend":true}},"jsonrpc":"2.0","id":'$((n * 10 + 1))'}}}' ;;
        11) printf '%s\n' '{"type":"control_request","request_id":"q'$n'a","request":{"subtype":"mcp_message","server_name":"qucs","message":{"method":"tools/call","params":{"name":"add_trace","arguments":{"diagram":1,"variable":"R1.1","style":"arrows","axis":"right","marker":"plus","thickness":1000}},"jsonrpc":"2.0","id":'$((n * 10))'}}}'
           printf '%s\n' '{"type":"control_request","request_id":"q'$n'b","request":{"subtype":"mcp_message","server_name":"qucs","message":{"method":"tools/call","params":{"name":"edit_trace","arguments":{"diagram":1,"trace":1,"variable":"ngspice/x:y@z","color":"red","numbers":"nonsense"}},"jsonrpc":"2.0","id":'$((n * 10 + 1))'}}}' ;;
        12) printf '%s\n' '{"type":"control_request","request_id":"q'$n'a","request":{"subtype":"mcp_message","server_name":"qucs","message":{"method":"tools/call","params":{"name":"delete","arguments":{"traces":[{"diagram":1,"trace":99},{"diagram":"one"}],"diagrams":[1,1,0]}},"jsonrpc":"2.0","id":'$((n * 10))'}}}'
           printf '%s\n' '{"type":"control_request","request_id":"q'$n'b","request":{"subtype":"mcp_message","server_name":"qucs","message":{"method":"tools/call","params":{"name":"undo","arguments":{}},"jsonrpc":"2.0","id":'$((n * 10 + 1))'}}}' ;;
        13) printf '%s\n' '{"type":"control_request","request_id":"q'$n'a","request":{"subtype":"mcp_message","server_name":"qucs","message":{"method":"tools/call","params":{"name":"get_dataset","arguments":{"variables":["time","out",""],"points":5000,"at":[1e308,-1],"measure":["crossings","bandwidth","settling_time"],"from":1e308,"level":-1e308,"tolerance":-3}},"jsonrpc":"2.0","id":'$((n * 10))'}}}'
           printf '%s\n' '{"type":"control_request","request_id":"q'$n'b","request":{"subtype":"mcp_message","server_name":"qucs","message":{"method":"tools/call","params":{"name":"reload_data","arguments":{}},"jsonrpc":"2.0","id":'$((n * 10 + 1))'}}}' ;;
        14) printf '%s\n' '{"type":"control_request","request_id":"q'$n'a","request":{"subtype":"mcp_message","server_name":"qucs","message":{"method":"tools/call","params":{"name":"rename_net","arguments":{"from":"net1","to":"x'$n'"}},"jsonrpc":"2.0","id":'$((n * 10))'}}}'
           printf '%s\n' '{"type":"control_request","request_id":"q'$n'b","request":{"subtype":"mcp_message","server_name":"qucs","message":{"method":"tools/call","params":{"name":"set_label","arguments":{"at":[0,0],"name":""}},"jsonrpc":"2.0","id":'$((n * 10 + 1))'}}}' ;;
        15) printf '%s\n' '{"type":"control_request","request_id":"q'$n'a","request":{"subtype":"mcp_message","server_name":"qucs","message":{"method":"tools/call","params":{"name":"describe_component_type","arguments":{"type":"'$(echo Sub .TR Vpulse Eqn SPICE _BJT | cut -d" " -f$((n % 6 + 1)))'"}},"jsonrpc":"2.0","id":'$((n * 10))'}}}'
           printf '%s\n' '{"type":"control_request","request_id":"q'$n'b","request":{"subtype":"mcp_message","server_name":"qucs","message":{"method":"tools/call","params":{"name":"get_netlist","arguments":{"numbered":true,"last":'$([ $((n % 2)) -eq 0 ] && echo true || echo false)'}},"jsonrpc":"2.0","id":'$((n * 10 + 1))'}}}' ;;
        16) printf '%s\n' '{"type":"control_request","request_id":"q'$n'a","request":{"subtype":"mcp_message","server_name":"qucs","message":{"method":"tools/call","params":{"name":"add_marker","arguments":{"diagram":1,"trace":1,"at":"'$(echo peak -3dB min crossing:1e308 crossing:-0 nowhere | cut -d" " -f$((n % 6 + 1)))'","label":[2147483647,-2147483648],"precision":99,"fill_color":"#00000000"}},"jsonrpc":"2.0","id":'$((n * 10))'}}}'
           printf '%s\n' '{"type":"control_request","request_id":"q'$n'b","request":{"subtype":"mcp_message","server_name":"qucs","message":{"method":"tools/call","params":{"name":"edit_marker","arguments":{"diagram":1,"marker":'$((n % 3))',"at":1e308,"label_offset":[99999999,-99999999],"format":"magnitude_radians"}},"jsonrpc":"2.0","id":'$((n * 10 + 1))'}}}' ;;
        17) printf '%s\n' '{"type":"control_request","request_id":"q'$n'a","request":{"subtype":"mcp_message","server_name":"qucs","message":{"method":"tools/call","params":{"name":"delete_marker","arguments":{"diagram":1,"marker":1}},"jsonrpc":"2.0","id":'$((n * 10))'}}}'
           printf '%s\n' '{"type":"control_request","request_id":"q'$n'b","request":{"subtype":"mcp_message","server_name":"qucs","message":{"method":"tools/call","params":{"name":"move_to_pane","arguments":{"pane":"'$(echo right below 1 2 9 left | cut -d" " -f$((n % 6 + 1)))'"}},"jsonrpc":"2.0","id":'$((n * 10 + 1))'}}}' ;;
        18) printf '%s\n' '{"type":"control_request","request_id":"q'$n'a","request":{"subtype":"mcp_message","server_name":"qucs","message":{"method":"tools/call","params":{"name":"export_netlist","arguments":{"save_as":"'"$QUCS_FAKE_EDIT"'.cir","format":"'$(echo spice cdl | cut -d" " -f$((n % 2 + 1)))'"}},"jsonrpc":"2.0","id":'$((n * 10))'}}}'
           printf '%s\n' '{"type":"control_request","request_id":"q'$n'b","request":{"subtype":"mcp_message","server_name":"qucs","message":{"method":"tools/call","params":{"name":"get_schematic","arguments":{"properties":"all","components":["R1","",7],"region":[2147483647,-2147483648,0,0]}},"jsonrpc":"2.0","id":'$((n * 10 + 1))'}}}' ;;
        19) printf '%s\n' '{"type":"control_request","request_id":"q'$n'a","request":{"subtype":"mcp_message","server_name":"qucs","message":{"method":"tools/call","params":{"name":"get_dataset","arguments":{"operating_point":true,"decibels":true}},"jsonrpc":"2.0","id":'$((n * 10))'}}}'
           printf '%s\n' '{"type":"control_request","request_id":"q'$n'b","request":{"subtype":"mcp_message","server_name":"qucs","message":{"method":"tools/call","params":{"name":"screenshot","arguments":{"area":"window"}},"jsonrpc":"2.0","id":'$((n * 10 + 1))'}}}' ;;
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
