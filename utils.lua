utils = {}

function utils.PrintTable(tbl, indent, visited)
    indent = indent or 0
    visited = visited or {}
    
    if type(tbl) ~= "table" then
        print(string.rep("  ", indent) .. tostring(tbl) .. " (" .. type(tbl) .. ")")
        return
    end
    
    -- Prevent infinite recursion with circular references
    if visited[tbl] then
        print(string.rep("  ", indent) .. "[Circular Reference]")
        return
    end
    visited[tbl] = true
    
    -- Count total entries for better formatting
    local count = 0
    for _ in pairs(tbl) do count = count + 1 end
    
    if count == 0 then
        print(string.rep("  ", indent) .. "{} (empty table)")
        return
    end
    
    print(string.rep("  ", indent) .. "{")
    
    -- Sort keys for consistent output (separate numeric and string keys)
    local numKeys, strKeys = {}, {}
    for k in pairs(tbl) do
        if type(k) == "number" then
            table.insert(numKeys, k)
        else
            table.insert(strKeys, tostring(k))
        end
    end
    
    table.sort(numKeys)
    table.sort(strKeys)
    
    -- Print numeric keys first
    for _, k in ipairs(numKeys) do
        local v = tbl[k]
        local valueType = type(v)
        local keyStr = string.rep("  ", indent + 1) .. "[" .. k .. "] = "
        
        if valueType == "table" then
            print(keyStr .. "table (" .. tostring(v) .. ")")
            PrintTable(v, indent + 2, visited)
        elseif valueType == "string" then
            print(keyStr .. '"' .. tostring(v) .. '" (string)')
        elseif valueType == "function" then
            print(keyStr .. "function (" .. tostring(v) .. ")")
        elseif valueType == "userdata" then
            print(keyStr .. "userdata (" .. tostring(v) .. ")")
        elseif valueType == "boolean" then
            print(keyStr .. tostring(v) .. " (boolean)")
        elseif valueType == "nil" then
            print(keyStr .. "nil")
        else
            print(keyStr .. tostring(v) .. " (" .. valueType .. ")")
        end
    end
    
    -- Print string keys
    for _, k in ipairs(strKeys) do
        local v = tbl[k]
        local valueType = type(v)
        local keyStr = string.rep("  ", indent + 1) .. k .. " = "
        
        if valueType == "table" then
            print(keyStr .. "table (" .. tostring(v) .. ")")
            utils.PrintTable(v, indent + 2, visited)
        elseif valueType == "string" then
            print(keyStr .. '"' .. tostring(v) .. '" (string)')
        elseif valueType == "function" then
            print(keyStr .. "function (" .. tostring(v) .. ")")
        elseif valueType == "userdata" then
            print(keyStr .. "userdata (" .. tostring(v) .. ")")
        elseif valueType == "boolean" then
            print(keyStr .. tostring(v) .. " (boolean)")
        elseif valueType == "nil" then
            print(keyStr .. "nil")
        else
            print(keyStr .. tostring(v) .. " (" .. valueType .. ")")
        end
    end
    
    print(string.rep("  ", indent) .. "}")
    
    -- Clean up visited tracking for this level
    visited[tbl] = nil
end

return utils