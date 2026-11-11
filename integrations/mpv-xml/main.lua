local utils = require 'mp.utils'
local dir = mp.get_script_directory()
if not dir then mp.msg.error('请将 DanmaX 拖放组件作为 scripts/danmax_drop 目录安装'); return end
local config_file = io.open(utils.join_path(dir, 'config.json'), 'rb')
if not config_file then return end
local config = utils.parse_json(config_file:read('*a')); config_file:close()
if not config or config.version ~= 1 or not config.executable or not config.dataDirectory then return end
local function message(text) mp.osd_message(text, 4); mp.msg.info(text) end
local function current_entry()
    for _, entry in ipairs(mp.get_property_native('playlist', {})) do
        if entry.current then return tostring(entry.id) end
    end
end
local function xml(path) return path:lower():match('%.xml$') ~= nil end
local function deliver(path)
    local entry = current_entry()
    if not entry or mp.get_property_bool('idle-active', true) then
        message('请先播放视频，再拖入 XML 弹幕'); return
    end
    local info = utils.file_info(path)
    if not info or not info.is_file then message('只支持本地 XML 弹幕文件'); return end
    local pipe = mp.get_property('input-ipc-server', '')
    if pipe == '' then
        pipe = '\\\\.\\pipe\\mpv-url-handler-xml-' .. tostring(utils.getpid())
        mp.set_property('input-ipc-server', pipe)
    end
    local args = {config.executable, '--data-dir', config.dataDirectory, '--mpv-xml', path,
                  '--mpv-pid', tostring(utils.getpid()), '--mpv-pipe', pipe, '--mpv-entry', entry}
    if config.diagnosticReport then args[#args + 1] = '--report'; args[#args + 1] = config.diagnosticReport end
    mp.command_native_async({name='subprocess', playback_only=false, detach=true, args=args},
        function(success, result)
            if not success or result.status ~= 0 then message('DanmaX 未能加载 XML，请查看 DanmaX 日志或重新安装接入') end
        end)
    message('正在交给 DanmaX 加载 XML 弹幕')
end
local function drop(action, ...)
    if mp.get_property('drag-and-drop') == 'no' then return end
    local paths = {...}; local count = 0
    for _, path in ipairs(paths) do if xml(path) then count = count + 1 end end
    if count > 0 then
        if #paths ~= 1 then message('请一次只拖入一个 XML 弹幕文件'); return end
        deliver(paths[1]); return
    end
    -- Retain mpv's ordinary subtitle/video/URL drop behavior.
    local extensions = mp.get_property_native('sub-auto-exts', {})
    local subtitles = #paths > 0
    for _, path in ipairs(paths) do
        local ext = path:lower():match('%.([^./\\]+)$'); local sub = false
        for _, value in ipairs(extensions) do if ext == value:lower() then sub = true end end
        subtitles = subtitles and sub
    end
    if subtitles then for _, path in ipairs(paths) do mp.commandv('sub-add', path) end; return end
    local mode = mp.get_property('drag-and-drop', 'auto'); if mode ~= 'auto' then action = mode end
    if action == 'insert-next' then
        for i = #paths, 1, -1 do mp.commandv('loadfile', paths[i], i == 1 and 'insert-next-play' or 'insert-next') end
    else
        for i, path in ipairs(paths) do mp.commandv('loadfile', path, i == 1 and action == 'replace' and 'replace' or 'append-play') end
    end
end
mp.register_script_message('drop', drop)
mp.register_script_message('source-loaded', function(count, entry)
    if entry ~= current_entry() then return end
    -- Only hide a known, currently enabled uosc_danmaku instance, and only
    -- after the external source loaded successfully. Other subtitles are intact.
    if mp.get_property_bool('user-data/uosc_danmaku/danmaku-switch-on', false) then
        mp.commandv('script-message-to', 'uosc_danmaku', 'show_danmaku_keyboard')
    end
    message('DanmaX 已加载 ' .. tostring(count) .. ' 条本地弹幕')
end)
mp.register_script_message('native-ready', function()
    mp.set_property_bool('user-data/danmax-xml-drop-ready', true)
    mp.msg.info('DanmaX XML drop ready')
end)
mp.register_script_message('native-error', function() message('DanmaX XML 拖放初始化失败') end)
mp.commandv('load-script', utils.join_path(dir, 'danmax_mpv_drop.dll'))
