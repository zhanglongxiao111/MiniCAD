#pragma once
#include "ICommand.h"
#include <stack>
#include <memory>
#include <functional>
namespace MiniCAD
{
    class Scene;
    class CommandStack
    {
    public:
        // 场景被命令修改时回调(Execute/Push/Undo/Redo)。
        // Document 用它统一标脏,各工具无需自行通知。
        void SetOnMutate(std::function<void()> fn) { m_onMutate = std::move(fn); }

        // 每条新命令(Execute/Push)生效后调用,返回已应用的附属改动(如关联标注跟随几何),
        // 与原命令合为一步撤销 / 重做;无附属改动返回空。Undo/Redo 时不调用(附属改动随命令重放)。
        using PostCommandHook = std::function<std::unique_ptr<ICommand>()>;
        void SetPostCommandHook(PostCommandHook fn) { m_postCommand = std::move(fn); }
        // 嵌入模式：全局否决钩子。命令生效后、入栈前调用，返回 true 则立即撤销该命令、不入栈（嵌入页只浏览 / 只批注时防误改）
        using VetoHook = std::function<bool(const ICommand&)>;
        static void SetVetoHook(VetoHook fn);
        // 嵌入模式：撤销/重做策略门禁。返回 false 时阻止执行与汇报可用（浏览态统一拦住 Ctrl+Shift+Z/Y 等直接入口）
        using UndoRedoGate = std::function<bool(const ICommand&, bool isRedo)>;
        static void SetUndoRedoGate(UndoRedoGate fn);
        void SetScene(Scene* scene) { m_scene = scene; }   // Push（已经发生的改动）被否决时用来撤销

        // Execute = 执行 + 入栈；Execute 返回 false 时不入栈
        bool Execute(std::unique_ptr<ICommand> cmd, Scene& scene);
        void Undo(Scene& scene);
        void Redo(Scene& scene);

        // Push = 只入栈，不执行（例如：拖拽这种"已经发生"的操作）
        void Push(std::unique_ptr<ICommand> cmd);
        bool CanUndo() const;
        bool CanRedo() const;

        void Clear();

    private:
        void NotifyMutate() { if (m_onMutate) m_onMutate(); }
        void PushNew(std::unique_ptr<ICommand> cmd);   // 入栈(附带 PostCommandHook 的改动)并清空 Redo

        std::stack<std::unique_ptr<ICommand>> m_undoStack;
        std::stack<std::unique_ptr<ICommand>> m_redoStack;
        std::function<void()>                 m_onMutate;
        PostCommandHook                       m_postCommand;
        Scene*                                m_scene = nullptr;
    };
}
