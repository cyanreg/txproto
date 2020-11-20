sp = {
    dump = function(node)
        local cache, stack, output = {},{},{}
        local depth = 1

        if type(node) ~= "table" then
            print(node)
            return nil
        end

        tree = true
        indentation = 4

        local NEW_LINE = "\n"
        local TAB_CHAR = " "

        if nil == tree then
            NEW_LINE = "\n"
        elseif not tree then
            NEW_LINE = ""
            TAB_CHAR = ""
        end

        local output_str = "{" .. NEW_LINE

        while true do
            local size = 0
            for k,v in pairs(node) do
                size = size + 1
            end

            local cur_index = 1
            for k,v in pairs(node) do
                if (cache[node] == nil) or (cur_index >= cache[node]) then

                    if (string.find(output_str,"}",output_str:len())) then
                        output_str = output_str .. "," .. NEW_LINE
                    elseif not (string.find(output_str,NEW_LINE,output_str:len())) then
                        output_str = output_str .. NEW_LINE
                    end

                    -- This is necessary for working with HUGE tables otherwise we run out of memory using concat on huge strings
                    table.insert(output,output_str)
                    output_str = ""

                    local key
                    if (type(k) == "number" or type(k) == "boolean") then
                        key = "["..tostring(k).."]"
                    else
                        key = "['"..tostring(k).."']"
                    end

                    if (type(v) == "number" or type(v) == "boolean") then
                        output_str = output_str .. string.rep(TAB_CHAR,depth*indentation) .. key .. " = "..tostring(v)
                    elseif (type(v) == "table") then
                        output_str = output_str .. string.rep(TAB_CHAR,depth*indentation) .. key .. " = {" .. NEW_LINE
                        table.insert(stack,node)
                        table.insert(stack,v)
                        cache[node] = cur_index+1
                        break
                    else
                        output_str = output_str .. string.rep(TAB_CHAR,depth*indentation) .. key .. " = '"..tostring(v).."'"
                    end

                    if (cur_index == size) then
                        output_str = output_str .. NEW_LINE .. string.rep(TAB_CHAR,(depth-1)*indentation) .. "}"
                    else
                        output_str = output_str .. ","
                    end
                else
                    -- close the table
                    if (cur_index == size) then
                        output_str = output_str .. NEW_LINE .. string.rep(TAB_CHAR,(depth-1)*indentation) .. "}"
                    end
                end

                cur_index = cur_index + 1
            end

            if (size == 0) then
                output_str = output_str .. NEW_LINE .. string.rep(TAB_CHAR,(depth-1)*indentation) .. "}"
            end

            if (#stack > 0) then
                node = stack[#stack]
                stack[#stack] = nil
                depth = cache[node] == nil and depth + 1 or depth - 1
            else
                break
            end
        end

        -- This is necessary for working with HUGE tables otherwise we run out of memory using concat on huge strings
        table.insert(output,output_str)
        output_str = table.concat(output)

        print(output_str)
        return output_str
    end,
}
