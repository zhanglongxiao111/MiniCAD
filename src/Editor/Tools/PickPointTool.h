#pragma once
#include "Editor/Tools/ITool.h"
#include "Editor/EditorContext.h"
#include "Core/Math/Point3.hpp"
#include <functional>
#include <string>
#include <utility>

namespace MiniCAD
{
    // 单点拾取工具:左键拾取一个点后回调并结束,右键/ESC 取消。
    // 用于"带基点复制"等只需要用户指定一个点的轻交互。
    class PickPointTool : public ITool
    {
    public:
        // prompt：N_ 标记的原文，显示时翻译
        PickPointTool(std::string prompt,
                      std::function<void(const Math::Point3&)> onPicked,
                      std::function<void()> onCancelled = nullptr)
            : m_prompt(std::move(prompt))
            , m_onPicked(std::move(onPicked))
            , m_onCancelled(std::move(onCancelled))
        {
        }

        ~PickPointTool() override
        {
            NotifyCancelled();
        }

        bool OnInput(const EditorContext& ctx) override
        {
            const auto& e = ctx.event;

            if (e.IsLeftClick())
            {
                const Math::Point3 p = e.HasSnap
                    ? e.SnapWorld
                    : ctx.viewport.GetCamera().ScreenToWorld(e.MouseX, e.MouseY);

                m_done = true;
                if (m_onPicked) m_onPicked(p);
                if (OnFinished) OnFinished();
                return true;
            }

            if (e.IsRightClick() || e.IsCancel())
            {
                Cancel();
                return true;
            }

            return false;
        }

        void Cancel() override
        {
            NotifyCancelled();
            if (OnFinished) OnFinished();
        }

        std::string GetPrompt() const override { return Tr(m_prompt); }

    private:
        void NotifyCancelled()
        {
            if (m_done) return;
            m_done = true;
            if (m_onCancelled) m_onCancelled();
        }

        std::string                               m_prompt;
        bool                                      m_done = false;
        std::function<void(const Math::Point3&)> m_onPicked;
        std::function<void()>                    m_onCancelled;
    };
}
