#include "SpecialWorkspaceGesture.hpp"

#include "../../../../Compositor.hpp"
#include "../../../../state/WorkspaceState.hpp"
#include "../../../../desktop/state/FocusState.hpp"
#include "../../../../render/Renderer.hpp"

#include "../../../../animation/WorkspaceAnimationController.hpp"
#include <cmath>

#include <hyprutils/memory/Casts.hpp>
using namespace Hyprutils::Memory;

constexpr const float MAX_DISTANCE = 150.F;

//
static Vector2D lerpVal(const Vector2D& from, const Vector2D& to, const float& t) {
    return Vector2D{
        from.x + ((to.x - from.x) * t),
        from.y + ((to.y - from.y) * t),
    };
}

CSpecialWorkspaceGesture::CSpecialWorkspaceGesture(const std::string& workspaceName) : m_specialWorkspaceName(workspaceName) {
    ;
}

void CSpecialWorkspaceGesture::begin(const ITrackpadGesture::STrackpadGestureBegin& e) {
    ITrackpadGesture::begin(e);

    m_specialWorkspace.reset();
    m_lastDelta = 0.F;
    m_monitor.reset();

    m_specialWorkspace = State::workspaceState()->query().name(std::format("special:{}", m_specialWorkspaceName)).run();

    // Direction is intentional: up opens only, down closes only.
    if (!m_specialWorkspace && e.direction == TRACKPAD_GESTURE_DIR_DOWN)
        return;

    if (m_specialWorkspace) {
        m_animatingOut = m_specialWorkspace->isVisible();

        if ((e.direction == TRACKPAD_GESTURE_DIR_UP && m_animatingOut) ||
            (e.direction == TRACKPAD_GESTURE_DIR_DOWN && !m_animatingOut)) {
            m_specialWorkspace.reset();
            return;
        }

        m_monitor = m_animatingOut ? m_specialWorkspace->m_monitor : Desktop::focusState()->monitor();

        if (!m_monitor)
            return;

        if (!m_animatingOut) {
            // Reopening may interrupt an in-flight close. setSpecialWorkspace()
            // establishes the configured open targets, then restore the current
            // workspace position so the animation reverses without teleporting.
            const auto CURR_WS_ALPHA  = m_specialWorkspace->m_alpha->value();
            const auto CURR_WS_OFFSET = m_specialWorkspace->m_renderOffset->value();

            m_monitor->setSpecialWorkspace(m_specialWorkspace);

            m_specialWorkspace->m_alpha->setValue(CURR_WS_ALPHA);
            m_specialWorkspace->m_renderOffset->setValue(CURR_WS_OFFSET);
        } else {
            // Freeze an in-flight open animation at its current value. begun()
            // is the animation's old start (often the hidden start position),
            // not the position under the user's fingers.
            const auto CURR_WS_ALPHA  = m_specialWorkspace->m_alpha->value();
            const auto CURR_WS_OFFSET = m_specialWorkspace->m_renderOffset->value();
            const auto CURR_MON_FADE  = m_monitor->m_specialFade->value();
            const auto CURR_MON_DIM   = m_monitor->m_specialDim->value();
            const auto CURR_MON_BLUR  = m_monitor->m_specialBlur->value();
            const auto OPEN_MON_FADE  = m_monitor->m_specialFade->goal();
            const auto OPEN_MON_DIM   = m_monitor->m_specialDim->goal();
            const auto OPEN_MON_BLUR  = m_monitor->m_specialBlur->goal();

            m_specialWorkspace->m_alpha->setValueAndWarp(CURR_WS_ALPHA);
            m_specialWorkspace->m_renderOffset->setValueAndWarp(CURR_WS_OFFSET);
            m_monitor->m_specialFade->setValueAndWarp(CURR_MON_FADE);
            m_monitor->m_specialDim->setValueAndWarp(CURR_MON_DIM);
            m_monitor->m_specialBlur->setValueAndWarp(CURR_MON_BLUR);

            // Ask the normal animation controller for the exact configured
            // close target, but preserve the current value (no teleport).
            Animation::Workspace::startAnimation(
                m_specialWorkspace,
                Animation::Workspace::ANIMATION_TYPE_OUT,
                false,
                false,
                std::nullopt,
                true);

            m_monitorFadeFrom     = CURR_MON_FADE;
            m_monitorFadeTo       = OPEN_MON_FADE;
            m_monitorDimFrom      = CURR_MON_DIM;
            m_monitorDimTo        = OPEN_MON_DIM;
            m_monitorBlurFrom     = CURR_MON_BLUR;
            m_monitorBlurTo       = OPEN_MON_BLUR;
            m_workspaceAlphaFrom  = CURR_WS_ALPHA;
            m_workspaceAlphaTo    = m_specialWorkspace->m_alpha->goal();
            m_workspaceOffsetFrom = CURR_WS_OFFSET;
            m_workspaceOffsetTo   = m_specialWorkspace->m_renderOffset->goal();

            // Keep the workspace at the touch-down position until update()
            // supplies the first real finger delta.
            m_specialWorkspace->m_alpha->setValueAndWarp(CURR_WS_ALPHA);
            m_specialWorkspace->m_renderOffset->setValueAndWarp(CURR_WS_OFFSET);
            return;
        }
    } else {
        m_monitor = Desktop::focusState()->monitor();

        if (!m_monitor)
            return;

        m_animatingOut = false;

        const auto& [workspaceID, workspaceName, isAutoID] = getWorkspaceIDNameFromString(std::format("special:{}", m_specialWorkspaceName));
        const auto WS                                      = State::workspaceState()->create(workspaceID, m_monitor->m_id, workspaceName);
        m_monitor->setSpecialWorkspace(WS);
        m_specialWorkspace = WS;
    }

    if (!m_specialWorkspace)
        return;

    m_monitorFadeFrom     = m_monitor->m_specialFade->begun();
    m_monitorFadeTo       = m_monitor->m_specialFade->goal();
    m_monitorDimFrom      = m_monitor->m_specialDim->begun();
    m_monitorDimTo        = m_monitor->m_specialDim->goal();
    m_monitorBlurFrom     = m_monitor->m_specialBlur->begun();
    m_monitorBlurTo       = m_monitor->m_specialBlur->goal();
    m_workspaceAlphaFrom  = m_specialWorkspace->m_alpha->begun();
    m_workspaceAlphaTo    = m_specialWorkspace->m_alpha->goal();
    m_workspaceOffsetFrom = m_specialWorkspace->m_renderOffset->begun();
    m_workspaceOffsetTo   = m_specialWorkspace->m_renderOffset->goal();
}

void CSpecialWorkspaceGesture::update(const ITrackpadGesture::STrackpadGestureUpdate& e) {
    if (!m_specialWorkspace || !m_monitor)
        return;

    g_pHyprRenderer->damageMonitor(m_specialWorkspace->m_monitor.lock());

    m_lastDelta += distance(e);

    const auto FADEPERCENT   = std::clamp(m_lastDelta / MAX_DISTANCE, 0.F, 1.F);
    const auto MON_FADE_TO   = m_animatingOut ? 0.F : m_monitorFadeTo;
    const auto MON_DIM_TO    = m_animatingOut ? 0.F : m_monitorDimTo;
    const auto MON_BLUR_TO   = m_animatingOut ? 0.F : m_monitorBlurTo;

    m_monitor->m_specialFade->setValueAndWarp(std::lerp(m_monitorFadeFrom, MON_FADE_TO, FADEPERCENT));
    m_monitor->m_specialDim->setValueAndWarp(std::lerp(m_monitorDimFrom, MON_DIM_TO, FADEPERCENT));
    m_monitor->m_specialBlur->setValueAndWarp(std::lerp(m_monitorBlurFrom, MON_BLUR_TO, FADEPERCENT));
    m_specialWorkspace->m_alpha->setValueAndWarp(std::lerp(m_workspaceAlphaFrom, m_workspaceAlphaTo, FADEPERCENT));
    m_specialWorkspace->m_renderOffset->setValueAndWarp(lerpVal(m_workspaceOffsetFrom, m_workspaceOffsetTo, FADEPERCENT));
}

void CSpecialWorkspaceGesture::end(const ITrackpadGesture::STrackpadGestureEnd& e) {
    if (!m_specialWorkspace || !m_monitor)
        return;

    const auto COMPLETION = std::clamp(m_lastDelta / MAX_DISTANCE, 0.F, 1.F);

    if (m_animatingOut) {
        if (COMPLETION < 0.3F) {
            // Cancel the close by restoring the configured open animation from
            // the exact finger position. This also restores the IN animation
            // config if the close interrupted an in-flight open.
            const auto CURR_WS_ALPHA  = m_specialWorkspace->m_alpha->value();
            const auto CURR_WS_OFFSET = m_specialWorkspace->m_renderOffset->value();

            *m_monitor->m_specialFade = m_monitorFadeTo;
            *m_monitor->m_specialDim  = m_monitorDimTo;
            *m_monitor->m_specialBlur = m_monitorBlurTo;
            Animation::Workspace::startAnimation(m_specialWorkspace, Animation::Workspace::ANIMATION_TYPE_IN, true);

            m_specialWorkspace->m_alpha->setValue(CURR_WS_ALPHA);
            m_specialWorkspace->m_renderOffset->setValue(CURR_WS_OFFSET);
        } else {
            // commit: preserve the finger position and let the normal close
            // path animate to the configured close target without teleporting
            m_monitor->setSpecialWorkspace(nullptr, false, true);
        }
        return;
    }

    if (COMPLETION < 0.3F) {
        // cancel the operation, which effectively means just flip the animation direction
        // also flip goals if animating in
        m_animatingOut = !m_animatingOut;

        if (m_animatingOut) {
            m_workspaceOffsetTo = m_workspaceOffsetFrom;
            m_workspaceAlphaTo  = m_workspaceAlphaFrom;
            m_monitorFadeTo     = m_monitorFadeFrom;
            m_monitorDimTo      = m_monitorDimFrom;
            m_monitorBlurTo     = m_monitorBlurFrom;
        }
    }

    if (m_animatingOut) {
        const auto CURR_WS_ALPHA  = m_specialWorkspace->m_alpha->value();
        const auto CURR_WS_OFFSET = m_specialWorkspace->m_renderOffset->value();
        const auto CURR_MON_FADE  = m_monitor->m_specialFade->value();
        const auto CURR_MON_DIM   = m_monitor->m_specialDim->value();
        const auto CURR_MON_BLUR  = m_monitor->m_specialBlur->value();

        m_monitor->setSpecialWorkspace(nullptr);

        const auto GOAL_WS_ALPHA  = m_specialWorkspace->m_alpha->goal();
        const auto GOAL_WS_OFFSET = m_specialWorkspace->m_renderOffset->goal();

        m_monitor->m_specialFade->setValueAndWarp(CURR_MON_FADE);
        m_monitor->m_specialDim->setValueAndWarp(CURR_MON_DIM);
        m_monitor->m_specialBlur->setValueAndWarp(CURR_MON_BLUR);
        m_specialWorkspace->m_alpha->setValueAndWarp(CURR_WS_ALPHA);
        m_specialWorkspace->m_renderOffset->setValueAndWarp(CURR_WS_OFFSET);

        *m_monitor->m_specialFade           = 0.F;
        *m_monitor->m_specialDim            = 0.F;
        *m_monitor->m_specialBlur           = 0.F;
        *m_specialWorkspace->m_alpha        = GOAL_WS_ALPHA;
        *m_specialWorkspace->m_renderOffset = GOAL_WS_OFFSET;
    } else {
        *m_monitor->m_specialFade           = m_monitorFadeTo;
        *m_monitor->m_specialDim            = m_monitorDimTo;
        *m_monitor->m_specialBlur           = m_monitorBlurTo;
        *m_specialWorkspace->m_renderOffset = m_workspaceOffsetTo;
        *m_specialWorkspace->m_alpha        = m_workspaceAlphaTo;
    }
}
