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
            if (auto onCancelled = TakeCancelled())
                onCancelled();
        }

        bool OnInput(const EditorContext& ctx) override
        {
            const auto& e = ctx.event;

            if (e.IsLeftClick())
            {
                const Math::Point3 p = e.HasSnap
                    ? e.SnapWorld
                    : ctx.viewport.GetCamera().ScreenToWorld(e.MouseX, e.MouseY);

                // 先完成内部状态清理并拷贝/移出回调，最后再通知外部，
                // 避免外部回调重入替换工具或销毁编辑器后继续访问 this 造成 UAF。
                m_done = true;
                auto onPicked   = std::move(m_onPicked);
                auto onFinished = OnFinished;
                m_onCancelled   = nullptr;
                if (onFinished) onFinished();
                if (onPicked)   onPicked(p);
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
            auto onCancelled = TakeCancelled();
            auto onFinished  = OnFinished;
            if (onFinished)  onFinished();
            if (onCancelled) onCancelled();
        }

        std::string GetPrompt() const override { return Tr(m_prompt); }

    private:
        std::function<void()> TakeCancelled()
        {
            if (m_done) return {};
            m_done = true;
            m_onPicked = nullptr;
            return std::move(m_onCancelled);
        }

        std::string                               m_prompt;
        bool                                      m_done = false;
        std::function<void(const Math::Point3&)> m_onPicked;
        std::function<void()>                    m_onCancelled;
    };
}
