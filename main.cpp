#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/debug/Log.hpp>
#include <hyprland/src/managers/LayoutManager.hpp>
#include <hyprland/src/managers/EventManager.hpp>
#include <hyprland/src/render/decorations/CHyprGroupBarDecoration.hpp>
#include <format>

inline HANDLE PHANDLE = nullptr;

std::vector<int> workspaces;

namespace Monocle {

template <typename... Args>
void log(eLogLevel level, std::format_string<Args...> fmt, Args&&... args) {
    auto msg = std::vformat(fmt.get(), std::make_format_args(args...));
    Debug::log(level, "[Monocle] {}", msg);
}

std::vector<PHLWINDOW> getWindowsOnWorkspace() {
    std::vector<PHLWINDOW> windows = {};

    for (auto& w : g_pCompositor->m_vWindows) {
        int workspaceID = w->workspaceID();
        int currentWorkspace = g_pCompositor->m_pLastMonitor->activeWorkspaceID();
        if (workspaceID == currentWorkspace)
            windows.push_back(w);
    }

    return windows;
}

void moveWindowIntoGroup(PHLWINDOW pWindow, PHLWINDOW pWindowInDirection) {
    if (pWindow->m_sGroupData.deny)
        return;

    // g_pLayoutManager->getCurrentLayout()->onWindowRemoved(pWindow); // This removes groupped property!

    static auto USECURRPOS = CConfigValue<Hyprlang::INT>("group:insert_after_current");
    pWindowInDirection     = *USECURRPOS ? pWindowInDirection : pWindowInDirection->getGroupTail();

    pWindowInDirection->insertWindowToGroup(pWindow);
    pWindowInDirection->setGroupCurrent(pWindow);
    pWindow->updateWindowDecos();
    // g_pLayoutManager->getCurrentLayout()->recalculateWindow(pWindow);
    g_pCompositor->focusWindow(pWindow);
    g_pCompositor->warpCursorTo(pWindow->middle());

    if (!pWindow->getDecorationByType(DECORATION_GROUPBAR))
        pWindow->addWindowDeco(makeUnique<CHyprGroupBarDecoration>(pWindow));
}

void createGroup(PHLWINDOW window) {
    if (window->m_sGroupData.deny) {
        Debug::log(LOG, "createGroup: window:{:x},title:{} is denied as a group, ignored", (uintptr_t)window, window->m_szTitle);
        return;
    }

    if (window->m_sGroupData.pNextWindow.expired()) {
        window->m_sGroupData.pNextWindow = window->m_pSelf;
        window->m_sGroupData.head        = true;
        window->m_sGroupData.locked      = false;
        window->m_sGroupData.deny        = false;

        window->addWindowDeco(makeUnique<CHyprGroupBarDecoration>(window));

        // if (window->m_pWorkspace) {
        //     window->m_pWorkspace->updateWindows();
        //     window->m_pWorkspace->updateWindowData();
        // }
        g_pLayoutManager->getCurrentLayout()->recalculateMonitor(window->monitorID());
        g_pCompositor->updateAllWindowsAnimatedDecorationValues();

        g_pEventManager->postEvent(SHyprIPCEvent{"togglegroup", std::format("1,{:x}", (uintptr_t)window)});
    }
}

void destroyGroup(PHLWINDOW window) {
    if (window->m_sGroupData.pNextWindow == window->m_pSelf) {
        if (window->m_eGroupRules & GROUP_SET_ALWAYS) {
            Debug::log(LOG, "destoryGroup: window:{:x},title:{} has rule [group set always], ignored", (uintptr_t)window, window->m_szTitle);
            return;
        }
        window->m_sGroupData.pNextWindow.reset();
        window->m_sGroupData.head = false;
        window->updateWindowDecos();
        // if (window->m_pWorkspace) {
        //     window->m_pWorkspace->updateWindows();
        //     window->m_pWorkspace->updateWindowData();
        // }
        g_pLayoutManager->getCurrentLayout()->recalculateMonitor(window->monitorID());
        g_pCompositor->updateAllWindowsAnimatedDecorationValues();

        g_pEventManager->postEvent(SHyprIPCEvent{"togglegroup", std::format("0,{:x}", (uintptr_t)window)});
        return;
    }

    std::string            addresses;
    PHLWINDOW              curr = window->m_pSelf.lock();
    std::vector<PHLWINDOW> members;
    do {
        const auto PLASTWIN = curr;
        curr                = curr->m_sGroupData.pNextWindow.lock();
        PLASTWIN->m_sGroupData.pNextWindow.reset();
        curr->setHidden(false);
        members.push_back(curr);

        addresses += std::format("{:x},", (uintptr_t)curr.get());
    } while (curr.get() != window.get());

    for (auto const& w : members) {
        if (w->m_sGroupData.head)
            g_pLayoutManager->getCurrentLayout()->onWindowRemoved(curr);
        w->m_sGroupData.head = false;
    }

    const bool GROUPSLOCKEDPREV        = g_pKeybindManager->m_bGroupsLocked;
    g_pKeybindManager->m_bGroupsLocked = true;
    for (auto const& w : members) {
        g_pLayoutManager->getCurrentLayout()->onWindowCreated(w);
        w->updateWindowDecos();
    }
    g_pKeybindManager->m_bGroupsLocked = GROUPSLOCKEDPREV;

    // if (window->m_pWorkspace) {
    //     window->m_pWorkspace->updateWindows();
    //     window->m_pWorkspace->updateWindowData();
    // }
    g_pLayoutManager->getCurrentLayout()->recalculateMonitor(window->monitorID());
    g_pCompositor->updateAllWindowsAnimatedDecorationValues();

    if (!addresses.empty())
        addresses.pop_back();
    g_pEventManager->postEvent(SHyprIPCEvent{"togglegroup", std::format("0,{}", addresses)});
}

}

SDispatchResult monocleOn(std::string arg) {
    const auto currentWindow = g_pCompositor->m_pLastWindow;

    int currentWorkspace = g_pCompositor->m_pLastMonitor->activeWorkspaceID();
    workspaces.push_back(currentWorkspace);

    std::vector<PHLWINDOW> windows = Monocle::getWindowsOnWorkspace();

    if (windows.empty()) {
        return SDispatchResult{};
    }

    auto firstWindow = windows[0];
    if (!firstWindow->m_sGroupData.pNextWindow) {
        Monocle::createGroup(firstWindow);
    }

    for (size_t i = 1; i < windows.size(); i++) {
        auto window1 = windows[i-1];
        auto window2 = windows[i];
        g_pCompositor->focusWindow(window2);
        Monocle::moveWindowIntoGroup(window2, window1);
    }

    g_pCompositor->focusWindow(currentWindow.lock());

    return SDispatchResult{};
}

SDispatchResult monocleOff(std::string arg) {
    int currentWorkspace = g_pCompositor->m_pLastMonitor->activeWorkspaceID();
    size_t toRemove = SIZE_MAX;
    for (size_t i = 0; i < workspaces.size(); i++) {
        if (workspaces[i] == currentWorkspace)
            toRemove = i;
    }
    if (toRemove != SIZE_MAX)
        workspaces.erase(workspaces.begin() + toRemove);

    if (g_pCompositor->m_pLastWindow.expired()) {
        return SDispatchResult{};
    }

    if (g_pCompositor->m_pLastWindow->m_sGroupData.pNextWindow) {
        auto window = g_pCompositor->m_pLastWindow.lock();
        Monocle::destroyGroup(window);
    }

    return SDispatchResult{};
}

SDispatchResult monocleToggle(std::string arg) {
    int currentWorkspace = g_pCompositor->m_pLastMonitor->activeWorkspaceID();
    if (std::find(workspaces.begin(), workspaces.end(), currentWorkspace) != workspaces.end()) 
        monocleOff("");
    else 
        monocleOn("");

    return SDispatchResult{};
}

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string HASH = __hyprland_api_get_hash();

    // ALWAYS add this to your plugins. It will prevent random crashes coming from
    // mismatched header versions.
    if (HASH != GIT_COMMIT_HASH) {
        HyprlandAPI::addNotification(PHANDLE, "[MyPlugin] Mismatched headers! Can't proceed.",
                                     CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[MyPlugin] Version mismatch");
    }

    HyprlandAPI::addDispatcherV2(PHANDLE, "monocle:on", ::monocleOn);
    HyprlandAPI::addDispatcherV2(PHANDLE, "monocle:off", ::monocleOff);
    HyprlandAPI::addDispatcherV2(PHANDLE, "monocle:toggle", ::monocleToggle);

    return {"MyPlugin", "An amazing plugin that is going to change the world!", "Me", "1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    // ...
}
