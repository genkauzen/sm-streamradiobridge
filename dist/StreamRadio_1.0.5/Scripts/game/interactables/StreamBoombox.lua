-- Stream Radio Core for Scrap Mechanic 1.0.x
--
-- The Lua side deliberately keeps URL state and synchronized transport state.
-- Actual HTTP/audio decoding must be supplied by a native 1.0.5 bridge. The
-- bridge contract is documented in Bridge/README_RU.md.

StreamBoombox = class()

StreamBoombox.maxParentCount = 1
StreamBoombox.maxChildCount = 0
StreamBoombox.connectionInput = sm.interactable.connectionType.logic + sm.interactable.connectionType.seated +
    (sm.interactable.connectionType.composite or 0)
StreamBoombox.connectionOutput = sm.interactable.connectionType.logic
StreamBoombox.colorNormal = sm.color.new("#315d78")
StreamBoombox.colorHighlight = sm.color.new("#4aa8cf")
StreamBoombox.componentType = "streamRadio"

local MAX_TRACKS = 32
local MAX_URL_LENGTH = 2048

local function trim(value)
    if type(value) ~= "string" then
        return ""
    end
    value = string.gsub(value, "^%s+", "")
    value = string.gsub(value, "%s+$", "")
    return value
end

local function formatTime(seconds)
    seconds = math.max(0, math.floor(tonumber(seconds) or 0))
    local minutes = math.floor(seconds / 60)
    local remainder = seconds % 60
    return string.format("%02d:%02d", minutes, remainder)
end

local function getHost(url)
    local lowerUrl = string.lower(url)
    -- Lua patterns do not support the '?' quantifier; handle both schemes explicitly.
    local host = string.match(lowerUrl, "^https://([^/%?#]+)")
        or string.match(lowerUrl, "^http://([^/%?#]+)")
    if not host then
        return nil
    end
    host = string.gsub(host, ":%d+$", "")
    host = string.gsub(host, "^www%.", "")
    return host
end

local function hostIs(host, domain)
    return host == domain or string.sub(host, -(#domain + 1)) == "." .. domain
end

local function normalizeUrl(value)
    local url = trim(value)
    local lowerUrl = string.lower(url)
    if string.find(url, "[\"\r\n]") then
        return nil
    end
    if string.sub(lowerUrl, 1, 8) ~= "https://" and string.sub(lowerUrl, 1, 7) ~= "http://" then
        url = "https://" .. url
    end
    if #url < 12 or #url > MAX_URL_LENGTH then
        return nil
    end

    local host = getHost(url)
    if not host then
        return nil
    end

    local allowed = hostIs(host, "youtube.com")
        or hostIs(host, "youtu.be")
        or hostIs(host, "music.youtube.com")
        or hostIs(host, "tiktok.com")
        or hostIs(host, "vk.com")
        or hostIs(host, "vkvideo.ru")

    if not allowed then
        return nil
    end
    return url
end

local function copyTrack(track)
    if not track then
        return nil
    end
    return { url = track.url, label = track.label or track.url }
end

function StreamBoombox:sv_payload()
    local tracks = {}
    for i, track in ipairs(self.sv.tracks or {}) do
        tracks[i] = copyTrack(track)
    end

    return {
        tracks = tracks,
        current = self.sv.current or 1,
        playing = self.sv.playing == true,
        position = self.sv.position or 0,
        loop = self.sv.loop == true,
        shuffle = self.sv.shuffle == true,
        audioReady = self.sv.audioReady == true,
        revision = self.sv.revision or 1
    }
end

function StreamBoombox:sv_broadcastState()
    self.network:sendToClients("cl_receiveState", self:sv_payload())
end

function StreamBoombox:sv_setRadioActive()
    -- Keep the interactable's native active flag aligned with PLAY/STOP.
    -- This makes the radio visibly/power-wise enabled for seat and logic
    -- integrations; audio decoding is still delegated to the native bridge.
    if self.interactable and self.interactable.setActive then
        pcall(function()
            self.interactable:setActive(self.sv.playing == true)
        end)
    end
end

function StreamBoombox:sv_sync()
    self:sv_setRadioActive()
    self.storage:save(self.sv)
    self:sv_broadcastState()
end

function StreamBoombox:sv_changed()
    self.sv.revision = (self.sv.revision or 0) + 1
    self:sv_sync()
end

function StreamBoombox:sv_currentTrack()
    return self.sv.tracks and self.sv.tracks[self.sv.current or 1] or nil
end

function StreamBoombox:sv_findTrack(url)
    for index, track in ipairs(self.sv.tracks or {}) do
        if track.url == url then
            return index
        end
    end
    return nil
end

function StreamBoombox:sv_add(value, playNow)
    local label = type(value) == "table" and trim(value.label) or ""
    local url = type(value) == "table" and value.url or value
    url = normalizeUrl(url)
    if not url then
        return false, "Нужна ссылка YouTube, TikTok или VK"
    end

    local index = self:sv_findTrack(url)
    if not index then
        if #self.sv.tracks >= MAX_TRACKS then
            return false, "Очередь заполнена"
        end
        if #label == 0 or #label > 180 then label = url end
        table.insert(self.sv.tracks, { url = url, label = label })
        index = #self.sv.tracks
    elseif label ~= "" and #label <= 180 then
        self.sv.tracks[index].label = label
    end

    if playNow then
        self.sv.current = index
        self.sv.position = 0
        self.sv.playing = true
        self.sv.audioReady = false
    elseif not self:sv_currentTrack() then
        self.sv.current = index
    end
    return true, nil
end

function StreamBoombox.server_onCreate(self)
    print("[StreamRadio] server_onCreate uuid=" .. tostring(self.shape.uuid))
    self.sv = self.storage:load() or {}
    self.sv.tracks = self.sv.tracks or {}
    self.sv.current = self.sv.current or 1
    self.sv.playing = self.sv.playing == true
    self.sv.position = self.sv.position or 0
    self.sv.loop = self.sv.loop == true
    self.sv.shuffle = self.sv.shuffle == true
    self.sv.revision = self.sv.revision or 1
    -- The bridge is process-local. Wait for its first decoded buffer after a
    -- world load instead of advancing the shared clock during URL resolving.
    self.sv.audioReady = false
    self.sv.saveTimer = 0
    self.sv.clockTimer = 0
    self:sv_setRadioActive()

    self.interactable.publicData = {
        sc_component = {
            type = StreamBoombox.componentType,
            api = {
                getState = function()
                    return self:sv_payload()
                end,
                play = function()
                    self.sv.playing = true
                    self.sv.audioReady = false
                    self:sv_changed()
                end,
                stop = function()
                    self.sv.playing = false
                    self.sv.audioReady = false
                    self:sv_changed()
                end,
                next = function()
                    self:sv_next()
                end
            }
        }
    }
end

function StreamBoombox.server_onUnload(self)
    if self.sv then
        self.storage:save(self.sv)
    end
end

function StreamBoombox.server_onFixedUpdate(self, dt)
    dt = dt or (1 / 40)
    self.sv.clockTimer = (self.sv.clockTimer or 0) + dt
    if self.sv.playing and self:sv_currentTrack() and self.sv.audioReady then
        self.sv.position = (self.sv.position or 0) + dt
        self.sv.saveTimer = (self.sv.saveTimer or 0) + dt
        if self.sv.saveTimer >= 3 then
            self.sv.saveTimer = 0
            self.storage:save(self.sv)
        end
    end

    -- Keep already-connected clients close to the authoritative server clock
    -- without resending the full URL queue every tick.
    if self.sv.clockTimer >= 0.25 then
        self.sv.clockTimer = 0
        if self:sv_currentTrack() then
            self.network:sendToClients("cl_receiveClock", {
                current = self.sv.current,
                playing = self.sv.playing == true,
                position = self.sv.position or 0,
                loop = self.sv.loop == true,
                revision = self.sv.revision or 1
            })
        end
    end
end

function StreamBoombox:_sv_next()
    local count = #(self.sv.tracks or {})
    if count == 0 then
        return
    end

    if self.sv.shuffle and count > 1 then
        local nextIndex = self.sv.current
        while nextIndex == self.sv.current do
            nextIndex = math.random(1, count)
        end
        self.sv.current = nextIndex
    else
        self.sv.current = (self.sv.current % count) + 1
    end
    self.sv.position = 0
    self.sv.playing = true
    self.sv.audioReady = false
    self:sv_changed()
end

function StreamBoombox:_sv_previous()
    local count = #(self.sv.tracks or {})
    if count == 0 then
        return
    end

    if self.sv.position and self.sv.position > 5 then
        self.sv.position = 0
    else
        self.sv.current = ((self.sv.current - 2) % count) + 1
        self.sv.position = 0
    end
    self.sv.playing = true
    self.sv.audioReady = false
    self:sv_changed()
end

function StreamBoombox.sv_requestState(self, _, player)
    self.network:sendToClient(player, "cl_receiveState", self:sv_payload())
end

function StreamBoombox.sv_addAndPlay(self, value)
    local ok, errorText = self:sv_add(value, true)
    if ok then
        self:sv_changed()
    else
        self.network:sendToClients("cl_showStatus", errorText)
    end
end

function StreamBoombox.sv_queueTrack(self, value)
    local ok, errorText = self:sv_add(value, false)
    if ok then
        self:sv_changed()
    else
        self.network:sendToClients("cl_showStatus", errorText)
    end
end

function StreamBoombox.sv_toggle(self)
    if self:sv_currentTrack() then
        self.sv.playing = not self.sv.playing
        self.sv.audioReady = false
        self:sv_changed()
    end
end

function StreamBoombox.sv_stop(self)
    self.sv.playing = false
    self.sv.position = 0
    self.sv.audioReady = false
    self:sv_changed()
end

function StreamBoombox.sv_seek(self, value)
    value = tonumber(value)
    if not value or not self:sv_currentTrack() then
        return
    end
    self.sv.position = math.max(0, value)
    self.sv.audioReady = false
    self:sv_changed()
end

function StreamBoombox.sv_next(self)
    self:_sv_next()
end

function StreamBoombox.sv_previous(self)
    self:_sv_previous()
end

function StreamBoombox.sv_removeCurrent(self)
    if #self.sv.tracks == 0 then
        return
    end
    table.remove(self.sv.tracks, self.sv.current)
    if #self.sv.tracks == 0 then
        self.sv.current = 1
        self.sv.playing = false
        self.sv.position = 0
        self.sv.audioReady = false
    else
        self.sv.current = math.min(self.sv.current, #self.sv.tracks)
        self.sv.position = 0
        self.sv.audioReady = false
    end
    self:sv_changed()
end

function StreamBoombox.sv_trackEnded(self, value)
    if type(value) ~= "table" then return end
    local track = self:sv_currentTrack()
    if not self.sv.playing or not track
        or tonumber(value.current) ~= self.sv.current
        or tonumber(value.revision) ~= self.sv.revision
        or value.url ~= track.url then
        return
    end
    if self.sv.loop then
        self.sv.position = 0
        self.sv.audioReady = false
        self:sv_changed()
    else
        self:_sv_next()
    end
end

function StreamBoombox.sv_audioReady(self, value)
    if type(value) ~= "table" then
        return
    end
    local current = tonumber(value.current)
    local url = type(value.url) == "string" and value.url or nil
    local track = self:sv_currentTrack()
    if not self.sv.playing or not track or current ~= self.sv.current or track.url ~= url
        or (value.revision ~= nil and tonumber(value.revision) ~= self.sv.revision) then
        return
    end
    if not self.sv.audioReady then
        self.sv.audioReady = true
        self:sv_sync()
    end
end

function StreamBoombox.sv_toggleLoop(self)
    self.sv.loop = not self.sv.loop
    self:sv_sync()
end

function StreamBoombox.sv_toggleShuffle(self)
    self.sv.shuffle = not self.sv.shuffle
    self:sv_sync()
end

-- CLIENT --------------------------------------------------------------------

function StreamBoombox.client_onCreate(self)
    print("[StreamRadio] client_onCreate uuid=" .. tostring(self.shape.uuid))
    self.cl = {
        tracks = {},
        current = 1,
        playing = false,
        position = 0,
        loop = false,
        shuffle = false,
        -- Scrap Mechanic's native radio level is reached at the first slider
        -- percent; higher values remain available for louder vehicles.
        volume = 0.01,
        urlInput = "",
        editing = false,
        wasSeatButtonActive = false,
        gui = nil,
        guiTimer = 0,
        bridgeTimer = 0,
        previewTrackUrl = nil,
        previewPath = nil,
        duration = nil,
        durationUrl = nil,
        bridgeStatus = nil,
        audioReady = false,
        readyUrl = nil,
        revision = 1,
        endedReportedRevision = nil,
        page = "radio",
        searchQuery = "",
        searchResults = {},
        searchStatus = "Введите название видео",
        searchBusy = false,
        updatingSeek = false,
        statusText = nil,
        statusTimer = 0
    }
    self.network:sendToServer("sv_requestState")
end

function StreamBoombox:cl_isSeatParent(parent)
    local ok, connectionType = pcall(function()
        return parent:getConnectionOutputType()
    end)
    if ok and connectionType == sm.interactable.connectionType.seated then
        return true
    end

    local characterOk, character = pcall(function()
        return parent:getSeatCharacter()
    end)
    return characterOk and character ~= nil
end

function StreamBoombox:cl_getSingleParent()
    local ok, parent = pcall(function()
        return self.interactable:getSingleParent()
    end)
    if ok then
        return parent
    end
    return nil
end

function StreamBoombox:cl_getBridge()
    -- A native extension may expose this global without changing the mod API.
    -- Scrap Mechanic removes rawget/rawset from the mod sandbox, so use the
    -- ordinary global lookup here. Missing globals simply evaluate to nil.
    local bridge = StreamRadioBridge
    if bridge then
        return bridge
    end
    -- Scrap Mechanic may execute mod chunks in a sandbox that does not expose
    -- newly-created VM globals. The native bridge mirrors itself on `sm` for
    -- that case.
    if sm and type(sm) == "table" then
        return sm.StreamRadioBridge
    end
    return nil
end

function StreamBoombox:cl_bridgeReady()
    local bridge = self:cl_getBridge()
    return bridge ~= nil and type(bridge.update) == "function"
end

function StreamBoombox:cl_updatePreview(track)
    if not self.cl.gui or not self.cl.gui:isActive() then
        return
    end

    local url = track and track.url or nil
    local bridge = self:cl_getBridge()
    local hasPreviewProvider = bridge ~= nil and type(bridge.getPreviewImage) == "function"

    -- Avoid asking the native bridge for the same thumbnail every GUI refresh,
    -- but allow a bridge loaded later in the session to retry.
    if self.cl.previewTrackUrl == url and (self.cl.previewPath or not hasPreviewProvider) then
        return
    end

    self.cl.previewTrackUrl = url
    self.cl.previewPath = nil
    pcall(function()
        self.cl.gui:setIconImage("PreviewImage", self.shape.uuid)
    end)

    if not url or not hasPreviewProvider then
        return
    end

    -- The provider must return a local game-readable image path. Lua itself
    -- cannot download a remote thumbnail from YouTube/TikTok/VK.
    local ok, imagePath = pcall(bridge.getPreviewImage, bridge, url)
    if ok and type(imagePath) == "string" and imagePath ~= "" then
        local imageOk = pcall(function()
            self.cl.gui:setImage("PreviewImage", imagePath)
        end)
        if imageOk then
            self.cl.previewPath = imagePath
            self.cl.gui:setText("PreviewPlaceholder", "")
        end
    end
end

function StreamBoombox:cl_bridgeTick()
    local bridge = self:cl_getBridge()
    if not bridge or type(bridge.update) ~= "function" then
        self.cl.audioReady = false
        return
    end

    if type(bridge.getVolume) == "function" then
        local volumeOk, savedVolume = pcall(bridge.getVolume, bridge)
        if volumeOk and tonumber(savedVolume) then
            self.cl.volume = math.max(0, math.min(1, tonumber(savedVolume)))
        end
    end

    local track = self.cl.tracks[self.cl.current]
    local function vectorTable(value)
        if not value then
            return nil
        end
        return { x = value.x or 0, y = value.y or 0, z = value.z or 0 }
    end

    local radioPosition, radioVelocity
    local shapeOk, shape = pcall(function()
        return self.interactable:getShape()
    end)
    if shapeOk and shape then
        local positionOk, position = pcall(function()
            return shape:getWorldPosition()
        end)
        if positionOk then
            radioPosition = vectorTable(position)
        end
        local velocityOk, velocity = pcall(function()
            return shape:getVelocity()
        end)
        if velocityOk then
            radioVelocity = vectorTable(velocity)
        end
    end

    local listenerPosition, listenerVelocity, listenerForward, listenerUp
    local cameraOk, cameraPosition, cameraDirection, cameraUp = pcall(function()
        return sm.camera.getPosition(), sm.camera.getDirection(), sm.camera.getUp()
    end)
    if cameraOk then
        listenerPosition = vectorTable(cameraPosition)
        listenerForward = vectorTable(cameraDirection)
        listenerUp = vectorTable(cameraUp)
    end

    local playerOk, player = pcall(function()
        return sm.localPlayer.getPlayer()
    end)
    if playerOk and player then
        local characterOk, character = pcall(function()
            return player:getCharacter()
        end)
        if characterOk and character then
            local positionOk, position = pcall(function()
                return character:getWorldPosition()
            end)
            if positionOk and not listenerPosition then
                listenerPosition = vectorTable(position)
            end
            local velocityOk, velocity = pcall(function()
                return character:getVelocity()
            end)
            if velocityOk then
                listenerVelocity = vectorTable(velocity)
            end
        end
    end

    local payload = {
        url = track and track.url or nil,
        playing = self.cl.playing,
        position = self.cl.position,
        revision = self.cl.revision,
        volume = self.cl.volume,
        loop = self.cl.loop,
        radioPosition = radioPosition,
        radioVelocity = radioVelocity,
        listenerPosition = listenerPosition,
        listenerVelocity = listenerVelocity,
        listenerForward = listenerForward,
        listenerUp = listenerUp,
        interactable = self.interactable
    }
    local ok, result = pcall(bridge.update, bridge, payload)
    if ok and type(result) == "table" then
        local duration = tonumber(result.duration)
        if duration and duration > 0 then
            self.cl.duration = duration
            self.cl.durationUrl = payload.url
        end
        if type(result.status) == "string" and result.status ~= "" then
            self.cl.bridgeStatus = result.status
            local wasReady = self.cl.audioReady
            self.cl.audioReady = result.ready == true
            if self.cl.audioReady and not wasReady and self.cl.readyUrl ~= payload.url then
                -- Scrap Mechanic accepts one payload argument after the RPC
                -- name. Sending current and URL separately aborts this fixed
                -- update callback, which made the native channel go stale
                -- exactly three seconds after playback began.
                local ready = {
                    current = self.cl.current,
                    url = payload.url,
                    revision = self.cl.revision
                }
                local sent = pcall(function()
                    self.network:sendToServer("sv_audioReady", ready)
                end)
                if sent then
                    self.cl.readyUrl = payload.url
                else
                    self.cl.bridgeStatus = "Ошибка синхронизации audioReady"
                end
            elseif not self.cl.audioReady then
                self.cl.readyUrl = nil
            end
        end
        if result.ended == true and self.cl.endedReportedRevision ~= self.cl.revision and payload.url then
            self.cl.endedReportedRevision = self.cl.revision
            self.network:sendToServer("sv_trackEnded", {
                current = self.cl.current,
                url = payload.url,
                revision = self.cl.revision
            })
        end
    end
end

function StreamBoombox.client_onFixedUpdate(self, dt)
    dt = dt or (1 / 40)
    if self.cl.statusTimer and self.cl.statusTimer > 0 then
        self.cl.statusTimer = math.max(0, self.cl.statusTimer - dt)
        if self.cl.statusTimer == 0 then
            self.cl.statusText = nil
        end
    end
    self.cl.position = self.cl.position or 0
    if self.cl.playing and self.cl.audioReady and self.cl.tracks[self.cl.current] then
        self.cl.position = self.cl.position + dt
    end

    local parent = self:cl_getSingleParent()
    local active = self.interactable.active == true
    if parent and self:cl_isSeatParent(parent) and active and not self.cl.wasSeatButtonActive then
        self:openGui()
    end
    self.cl.wasSeatButtonActive = active

    self.cl.bridgeTimer = (self.cl.bridgeTimer or 0) + dt
    if self.cl.bridgeTimer >= 0.05 then
        self.cl.bridgeTimer = 0
        self:cl_bridgeTick()
    end

    self.cl.guiTimer = (self.cl.guiTimer or 0) + dt
    if self.cl.gui and self.cl.gui:isActive() and self.cl.guiTimer >= 0.2 then
        self.cl.guiTimer = 0
        self:cl_refreshGui()
    end
end

function StreamBoombox.client_canInteract(self)
    return true
end

function StreamBoombox.client_onInteract(self, _, state)
    if state then
        print("[StreamRadio] client_onInteract")
        self:openGui()
    end
end

function StreamBoombox:cl_createGui()
    self.cl.gui = sm.gui.createGuiFromLayout("$CONTENT_DATA/Gui/Layouts/StreamRadio.layout")
    self.cl.gui:setIconImage("PreviewImage", self.shape.uuid)
    self.cl.gui:createHorizontalSlider("VolumeSlider", 100, self.cl.volume * 100, "cl_onVolumeSliderMoved")
    self.cl.gui:createHorizontalSlider("SeekSlider", 1000, 0, "cl_onSeekSliderMoved")

    self.cl.gui:setTextChangedCallback("UrlInput", "cl_onUrlChanged")
    self.cl.gui:setTextAcceptedCallback("UrlInput", "cl_onUrlAccepted")
    self.cl.gui:setButtonCallback("LoadButton", "cl_onLoad")
    self.cl.gui:setButtonCallback("QueueButton", "cl_onQueue")
    self.cl.gui:setButtonCallback("PreviousButton", "cl_onPrevious")
    self.cl.gui:setButtonCallback("PlayButton", "cl_onPlay")
    self.cl.gui:setButtonCallback("StopButton", "cl_onStop")
    self.cl.gui:setButtonCallback("NextButton", "cl_onNext")
    self.cl.gui:setButtonCallback("LoopButton", "cl_onLoop")
    self.cl.gui:setButtonCallback("RemoveButton", "cl_onRemove")
    self.cl.gui:setButtonCallback("RadioTabButton", "cl_onRadioTab")
    self.cl.gui:setButtonCallback("SearchTabButton", "cl_onSearchTab")
    self.cl.gui:setTextChangedCallback("SearchInput", "cl_onSearchChanged")
    self.cl.gui:setTextAcceptedCallback("SearchInput", "cl_onSearchAccepted")
    self.cl.gui:setButtonCallback("SearchButton", "cl_onSearch")
    self.cl.gui:setButtonCallback("SearchResult1", "cl_onSearchResult1")
    self.cl.gui:setButtonCallback("SearchResult2", "cl_onSearchResult2")
    self.cl.gui:setButtonCallback("SearchResult3", "cl_onSearchResult3")
    self.cl.gui:setButtonCallback("SearchResult4", "cl_onSearchResult4")
    self.cl.gui:setButtonCallback("SearchResult5", "cl_onSearchResult5")
    self.cl.gui:setButtonCallback("CloseButton", "cl_onClose")
    self.cl.gui:setOnCloseCallback("cl_onGuiClose")
    self:cl_setPage("radio")
end

function StreamBoombox:cl_setPage(page)
    if not self.cl.gui then
        return
    end
    self.cl.page = page == "search" and "search" or "radio"
    local searchPage = self.cl.page == "search"
    local radioWidgets = {
        "PreviewPanel", "InfoPanel", "TransportPanel", "VolumePanel", "Footer"
    }
    for _, name in ipairs(radioWidgets) do
        pcall(function()
            self.cl.gui:setVisible(name, not searchPage)
        end)
    end
    pcall(function() self.cl.gui:setVisible("SearchPanel", searchPage) end)
    pcall(function() self.cl.gui:setButtonState("RadioTabButton", not searchPage) end)
    pcall(function() self.cl.gui:setButtonState("SearchTabButton", searchPage) end)
end

function StreamBoombox:openGui()
    if not self.cl.gui then
        self:cl_createGui()
    end
    self.cl.editing = false
    self:cl_setPage(self.cl.page)
    self.cl.gui:setText("UrlInput", self.cl.urlInput or "")
    self.cl.gui:setText("SearchInput", self.cl.searchQuery or "")
    self.cl.gui:open()
    self:cl_refreshGui()
end

function StreamBoombox:cl_refreshGui()
    if not self.cl.gui or not self.cl.gui:isActive() then
        return
    end

    local track = self.cl.tracks[self.cl.current]
    if self.cl.page == "search" then
        self:cl_refreshSearch()
        return
    end
    local count = #self.cl.tracks
    if track then
        self.cl.gui:setText("TrackTitle", tostring(track.label or ("Трек " .. tostring(self.cl.current) .. "/" .. tostring(count))))
        if not self.cl.editing then
            self.cl.urlInput = track.url
            self.cl.gui:setText("UrlInput", self.cl.urlInput)
        end
        local previewReady = self.cl.previewTrackUrl == track.url and self.cl.previewPath ~= nil
        self.cl.gui:setText("PreviewPlaceholder", previewReady and "" or "ПРЕВЬЮ НЕДОСТУПНО\nАУДИО-РЕЖИМ")
        self:cl_updatePreview(track)
    else
        self.cl.gui:setText("TrackTitle", "Нет трека")
        self.cl.gui:setText("PreviewPlaceholder", "ВСТАВЬТЕ URL\nАУДИО-РЕЖИМ")
        self:cl_updatePreview(nil)
    end

    local bridgeReady = self:cl_bridgeReady()
    if self.cl.statusText and self.cl.statusTimer > 0 then
        self.cl.gui:setText("Status", self.cl.statusText)
    elseif bridgeReady then
        self.cl.gui:setText("Status", self.cl.bridgeStatus or (self.cl.playing and "Играет audio-only" or "Пауза"))
    else
        self.cl.gui:setText("Status", "Звук недоступен: нет StreamRadioBridge 1.0.5")
    end
    self.cl.gui:setButtonState("PlayButton", self.cl.playing)
    self.cl.gui:setButtonState("LoopButton", self.cl.loop)
    self.cl.gui:setText("LoopState", self.cl.loop and "LOOP: ВКЛ" or "LOOP: ВЫКЛ")
    self.cl.gui:setSliderPosition("VolumeSlider", self.cl.volume * 100)

    local shownPosition = self.cl.audioReady and (self.cl.position or 0) or 0
    local duration = tonumber(self.cl.duration)
    if track and duration and duration > 0 then
        local progress = math.max(0, math.min(1, shownPosition / duration))
        self.cl.gui:setText("PositionLabel", self.cl.audioReady and
            (formatTime(shownPosition) .. " / " .. formatTime(duration)) or "ЗАГРУЗКА АУДИО...")
        self.cl.updatingSeek = true
        self.cl.gui:setSliderPosition("SeekSlider", progress * 1000)
        self.cl.updatingSeek = false
    elseif track then
        self.cl.gui:setText("PositionLabel", self.cl.audioReady and
            (formatTime(shownPosition) .. " / LIVE") or "ЗАГРУЗКА АУДИО...")
        self.cl.updatingSeek = true
        self.cl.gui:setSliderPosition("SeekSlider", 0)
        self.cl.updatingSeek = false
    else
        self.cl.gui:setText("PositionLabel", "00:00 / --:--")
        self.cl.updatingSeek = true
        self.cl.gui:setSliderPosition("SeekSlider", 0)
        self.cl.updatingSeek = false
    end
end

function StreamBoombox.cl_receiveState(self, state)
    if type(state) ~= "table" then
        return
    end
    self.cl.tracks = state.tracks or {}
    self.cl.current = state.current or 1
    self.cl.playing = state.playing == true
    self.cl.position = state.position or 0
    self.cl.loop = state.loop == true
    self.cl.shuffle = state.shuffle == true
    local oldRevision = self.cl.revision
    self.cl.revision = tonumber(state.revision) or self.cl.revision or 1
    if oldRevision ~= self.cl.revision then
        self.cl.endedReportedRevision = nil
    end
    local track = self.cl.tracks[self.cl.current]
    local url = track and track.url or nil
    if self.cl.durationUrl ~= url then
        self.cl.durationUrl = url
        self.cl.duration = nil
        self.cl.bridgeStatus = nil
        self.cl.audioReady = false
        self.cl.readyUrl = nil
    end
    if not self.cl.playing then
        self.cl.audioReady = false
        self.cl.readyUrl = nil
    end
    self:cl_refreshGui()
end

function StreamBoombox.cl_receiveClock(self, state)
    if type(state) ~= "table"
        or state.current ~= self.cl.current
        or (state.revision ~= nil and tonumber(state.revision) ~= self.cl.revision)
        or not self.cl.tracks[self.cl.current] then
        return
    end
    self.cl.playing = state.playing == true
    self.cl.position = tonumber(state.position) or self.cl.position or 0
    if state.loop ~= nil then
        self.cl.loop = state.loop == true
    end
    self:cl_refreshGui()
end

function StreamBoombox.cl_showStatus(self, text)
    self.cl.statusText = text or "Ошибка"
    self.cl.statusTimer = 4
    if self.cl.gui and self.cl.gui:isActive() then
        self.cl.gui:setText("Status", self.cl.statusText)
    end
end

function StreamBoombox.cl_onUrlChanged(self, editBoxName, text)
    -- Scrap Mechanic passes (self, editBoxName, text) to EditBox callbacks.
    -- Keep the fallback for older/custom GUI callback wrappers.
    if type(text) ~= "string" and type(editBoxName) == "string" then
        text = editBoxName
    end
    if type(text) == "string" then
        self.cl.urlInput = text
        self.cl.editing = true
    end
end

function StreamBoombox.cl_onUrlAccepted(self, editBoxName, text)
    if type(text) ~= "string" and type(editBoxName) == "string" then
        text = editBoxName
    end
    if type(text) == "string" then
        self.cl.urlInput = text
    end
    self.cl.editing = true
end

function StreamBoombox:cl_readUrlInput()
    if self.cl.gui then
        -- getText is available in the 1.0.5 GUI API. The protected call keeps
        -- compatibility with older Fant GUI wrappers that do not expose it.
        local ok, text = pcall(function()
            return self.cl.gui:getText("UrlInput")
        end)
        if ok and type(text) == "string" then
            self.cl.urlInput = text
        end
    end
    return self.cl.urlInput or ""
end

function StreamBoombox.cl_onLoad(self)
    self.cl.editing = false
    self.network:sendToServer("sv_addAndPlay", self:cl_readUrlInput())
end

function StreamBoombox.cl_onQueue(self)
    self.cl.editing = false
    self.network:sendToServer("sv_queueTrack", self:cl_readUrlInput())
end

function StreamBoombox.cl_onPrevious(self)
    self.network:sendToServer("sv_previous")
end

function StreamBoombox.cl_onPlay(self)
    self.network:sendToServer("sv_toggle")
end

function StreamBoombox.cl_onStop(self)
    self.network:sendToServer("sv_stop")
end

function StreamBoombox.cl_onNext(self)
    self.network:sendToServer("sv_next")
end

function StreamBoombox.cl_onLoop(self)
    -- Apply the visual state immediately. The authoritative value is still
    -- stored and broadcast by the server below.
    self.cl.loop = not self.cl.loop
    if self.cl.gui and self.cl.gui:isActive() then
        self.cl.gui:setButtonState("LoopButton", self.cl.loop)
        self.cl.gui:setText("LoopState", self.cl.loop and "LOOP: ВКЛ" or "LOOP: ВЫКЛ")
    end
    self.network:sendToServer("sv_toggleLoop")
end

function StreamBoombox.cl_onRemove(self)
    self.network:sendToServer("sv_removeCurrent")
end

function StreamBoombox.cl_onRadioTab(self)
    self:cl_setPage("radio")
    self:cl_refreshGui()
end

function StreamBoombox.cl_onSearchTab(self)
    self:cl_setPage("search")
    self:cl_refreshGui()
end

function StreamBoombox.cl_onSearchChanged(self, editBoxName, text)
    if type(text) ~= "string" and type(editBoxName) == "string" then text = editBoxName end
    if type(text) == "string" then self.cl.searchQuery = text end
end

function StreamBoombox.cl_onSearchAccepted(self, editBoxName, text)
    self:cl_onSearchChanged(editBoxName, text)
    self:cl_onSearch()
end

function StreamBoombox.cl_onSearch(self)
    if self.cl.gui then
        local ok, text = pcall(function() return self.cl.gui:getText("SearchInput") end)
        if ok and type(text) == "string" then self.cl.searchQuery = text end
    end
    self.cl.searchQuery = trim(self.cl.searchQuery)
    if self.cl.searchQuery == "" then
        self.cl.searchStatus = "Введите название видео"
        return
    end
    local bridge = self:cl_getBridge()
    if bridge and type(bridge.search) == "function" then
        self.cl.searchBusy = true
        self.cl.searchStatus = "Поиск YouTube..."
        pcall(bridge.search, bridge, self.cl.searchQuery)
    else
        self.cl.searchStatus = "Поиск ещё не готов"
    end
end

function StreamBoombox:cl_refreshSearch()
    local bridge = self:cl_getBridge()
    if bridge and type(bridge.search) == "function" and self.cl.searchQuery ~= "" then
        local ok, result = pcall(bridge.search, bridge, self.cl.searchQuery)
        if ok and type(result) == "table" then
            self.cl.searchBusy = result.busy == true
            self.cl.searchStatus = result.status or self.cl.searchStatus
            self.cl.searchResults = {}
            for index = 1, math.min(5, tonumber(result.count) or 0) do
                local url = result["url" .. tostring(index)]
                if type(url) == "string" and url ~= "" then
                    self.cl.searchResults[index] = {
                        url = url,
                        title = result["title" .. tostring(index)] or url,
                        thumbnail = result["thumbnail" .. tostring(index)]
                    }
                end
            end
        end
    end
    self.cl.gui:setText("SearchStatus", self.cl.searchStatus or "")
    for index = 1, 5 do
        local item = self.cl.searchResults[index]
        local caption = item and (tostring(index) .. ". " .. tostring(item.title)) or ""
        pcall(function() self.cl.gui:setText("SearchResult" .. tostring(index), caption) end)
        pcall(function() self.cl.gui:setVisible("SearchResult" .. tostring(index), item ~= nil) end)
    end
end

function StreamBoombox:cl_playSearchResult(index)
    local item = self.cl.searchResults[index]
    if not item then return end
    self.cl.urlInput = item.url
    self.cl.editing = false
    self.network:sendToServer("sv_addAndPlay", { url = item.url, label = item.title })
    self:cl_setPage("radio")
end

function StreamBoombox.cl_onSearchResult1(self) self:cl_playSearchResult(1) end
function StreamBoombox.cl_onSearchResult2(self) self:cl_playSearchResult(2) end
function StreamBoombox.cl_onSearchResult3(self) self:cl_playSearchResult(3) end
function StreamBoombox.cl_onSearchResult4(self) self:cl_playSearchResult(4) end
function StreamBoombox.cl_onSearchResult5(self) self:cl_playSearchResult(5) end

function StreamBoombox.cl_onVolumeSliderMoved(self, value)
    value = tonumber(value) or 100
    self.cl.volume = math.max(0, math.min(1, value / 100))
    local bridge = self:cl_getBridge()
    if bridge and type(bridge.setVolume) == "function" then
        pcall(bridge.setVolume, bridge, self.cl.volume)
    end
    self:cl_bridgeTick()
end

function StreamBoombox.cl_onSeekSliderMoved(self, value)
    if self.cl.updatingSeek then
        return
    end

    local duration = tonumber(self.cl.duration)
    if not duration or duration <= 0 or not self.cl.tracks[self.cl.current] then
        return
    end

    value = tonumber(value) or 0
    local position = duration * math.max(0, math.min(1000, value)) / 1000
    self.cl.position = position
    self.network:sendToServer("sv_seek", position)
    self:cl_bridgeTick()
end

function StreamBoombox.cl_onClose(self)
    if self.cl.gui then
        self.cl.gui:close()
    end
end

function StreamBoombox.cl_onGuiClose(self)
    self.cl.editing = false
end

function StreamBoombox.client_onDestroy(self)
    local bridge = self:cl_getBridge()
    if bridge and bridge.stop then
        pcall(bridge.stop, bridge, self.interactable)
    end
    if self.cl and self.cl.gui then
        self.cl.gui:destroy()
        self.cl.gui = nil
    end
end
