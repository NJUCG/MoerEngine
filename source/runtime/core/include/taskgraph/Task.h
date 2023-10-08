#ifndef TASK_H
#define TASK_H
#include <atomic>
#include "TaskGraph.h"
class TaskHandle {
public:
	TaskHandle() = default;
	~TaskHandle() {}
};
class TTask :TaskHandle {
public:
	TTask() = default;
	virtual ~TTask() {}

	//static SubsequentMode GetSubsequentMode() { return SubsequentMode::TrackSubsequents; }

};
//#include <chrono>
//#include <assert.h>
//#include <array>
//#include <MPMCQueue/include/rigtorp/MPMCQueue.h>
//#include <thread>
//#define INVALID_THREAD_ID std::numeric_limits<uint32_t>::max()
//enum class SubsequentMode {
//	TrackSubsequents,
//	FireAndForget
//};
//typedef std::chrono::milliseconds Timespan;
//
//class TaskBase;
//TaskBase* GetCurrentTask();
//// sets the current task and returns the previous current task
//TaskBase* ExchangeCurrentTask(TaskBase* Task);
//enum class EExtendedTaskPriority
//{
//	None,
//	Inline, // a task priority for "inline" task execution - a task is executed "inline" by the thread that unlocked it, w/o scheduling
//	TaskEvent, // a task priority used by task events, allows to shortcut task execution
//	Count
//};
//
//template<typename... TaskTypes>
//class TPrerequisites : public std::array<TaskBase*, sizeof...(TaskTypes)>
//{
//public:
//	TPrerequisites(TaskTypes&&... Tasks)
//	{
//		Fill(0, std::forward<TaskTypes>(Tasks)...);
//	}
//
//private:
//	template<typename FirstTaskType, typename... OtherTaskTypes>
//	void Fill(uint32 Index, FirstTaskType&& FirstTask, OtherTaskTypes&&... OtherTasks)
//	{
//		(*this)[Index] = FirstTask.Pimpl.GetReference();
//		Fill(Index + 1, std::forward<OtherTaskTypes>(OtherTasks)...);
//	}
//
//	template<typename TaskType>
//	void Fill(uint32 Index, TaskType&& Task)
//	{
//		(*this)[Index] = Task.Pimpl.GetReference();
//	}
//};
//
//template<typename... TaskTypes>
//TPrerequisites<TaskTypes...> Prerequisites(TaskTypes&... Tasks)
//{
//	return TPrerequisites<TaskTypes...>{ Forward<TaskTypes>(Tasks)... };
//}
//
//enum class ETaskPriority : int8_t
//{
//	High,
//	Normal,
//	Default = Normal,
//	ForegroundCount,
//	BackgroundHigh = ForegroundCount,
//	BackgroundNormal,
//	BackgroundLow,
//	Count,
//	Inherit, //Inherit the TaskPriority from the launching Task or the Default Priority if not launched from a Task.
//};
//
//template<typename Type, void (Type::* DeleteFunction)()>
//class TDeleter
//{
//	Type* Value;
//
//public:
//	inline TDeleter(Type* InValue) : Value(InValue)
//	{
//	}
//
//	inline TDeleter(const TDeleter&) = delete;
//	inline TDeleter(TDeleter&& Other) : Value(Other.Value)
//	{
//		Other.Value = nullptr;
//	}
//
//	inline Type* operator->() const
//	{
//		return Value;
//	}
//
//	inline ~TDeleter()
//	{
//		if (Value)
//		{
//			(Value->*DeleteFunction)();
//		}
//	}
//};
//
class TaskBase {
public:
	void destroy() {

	}
protected:
	TaskBase() = default;
	void initialize() {

	}
	virtual bool tryExecuteTask() = 0;
	virtual bool isComplete() { return false; }
	bool addSubsequents(TaskBase* target) {
		return m_subsequents.TryPush(target);
	}
	bool addPrerequests(TaskBase* target) {
		if (target != nullptr) {
			if (target->addSubsequents(this) && m_prerequests.TryPush(target)) {
				m_lock_num.fetch_add(1, std::memory_order_relaxed);
				return true;
			}
		}
		return false;
	}
	template<class TaskArrayType>
	void addPrerequests(TaskArrayType target) {
		int32_t added_count = 0;
		for (auto t:target)
		{
			TaskBase* task;
			if (std::is_same_v<TaskBase*, decltype(t)>) {
				task = t;
				if (task->addSubsequents(this)&& m_prerequests.TryPush(task)) {
					added_count++;
				}
			}
		}
		m_lock_num.fetch_add(added_count, std::memory_order_relaxed);
	}
	void tryLaunch() {
		
	}
	
protected:
	TaskHandle handle;
	StatMPSCQueue<TaskBase*> m_subsequents;
	StatMPSCQueue<TaskBase*> m_prerequests;
	std::atomic<uint32_t> m_lock_num;
};
//class TaskBase {
//
//	static constexpr uint32_t ExecutionFlag = 0x80000000;
//public:
//	void AddRef()
//	{
//		RefCount.fetch_add(1, std::memory_order_relaxed);
//	}
//
//	void Release()
//	{
//		uint32_t LocalRefCount = RefCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
//		if (LocalRefCount == 0)
//		{
//			delete this;
//		}
//	}
//
//	uint32_t GetRefCount() const
//	{
//		return RefCount.load(std::memory_order_relaxed);
//	}
//
//private:
//	std::atomic<uint32_t> RefCount;
//	////////////////////////////////////////////////////////////////////////////
//
//protected:
//	explicit TaskBase(uint32_t InitRefCount)
//		: RefCount(InitRefCount)
//	{
//	}
//
//	void Init(const char* InDebugName, ETaskPriority InPriority, EExtendedTaskPriority InExtendedPriority)
//	{
//		// store debug name, priority and an adaptor for task execution in low-level task. The task body can't be stored as this task implementation needs to do some accounting
//		// before the task is executed (e.g. maintainance of TLS "current task")
//		LowLevelTask.Init(InDebugName, InPriority,
//			[
//				this,
//				// releasing scheduler's task reference can cause task's automatic destruction and so must be done after the low-level task
//				// task is flagged as completed. The task is flagged as completed after the continuation is executed but before its destroyed.
//				// `Deleter` is captured by value and is destroyed along with the continuation, calling the given functor on destruction
//				Deleter = TDeleter<TaskBase, &TaskBase::Release>{ this }
//			]
//					{
//						TryExecuteTask();
//					}
//					);
//		ExtendedPriority = InExtendedPriority;
//	}
//
//	virtual ~TaskBase()
//	{
//		assert(IsCompleted());
//	}
//public:
//	// returns true if it's valid to wait for the task completion.
//	// it's not valid to wait for a task e.g. from inside task's execution, as this would deadlock
//	bool IsAwaitable() const
//	{
//		return *static_cast<unsigned int*>(static_cast<void*>(&std::this_thread::get_id())) != ExecutingThreadId.load(std::memory_order_relaxed);
//	}
//
//
//	// will be called to execute the task, must be implemented by a derived class that should call `FTaskBase::TryExecute` and pass the task body
//	// @see TExecutableTask::TryExecuteTask
//	virtual bool TryExecuteTask() = 0;
//
//	bool AddPrerequisites(TaskBase& Prerequisite)
//	{
//		assert(NumLocks.load(std::memory_order_relaxed) >= NumInitialLocks);
//		assert(NumLocks.load(std::memory_order_relaxed) < ExecutionFlag);
//
//		// registering the task as a subsequent of the given prerequisite can cause its immediate launch by the prerequisite
//		// (if the prerequisite has been completed on another thread), so we need to keep the task locked by assuming that the 
//		// prerequisite can be added successfully, and release the lock if it wasn't
//		uint32_t PrevNumLocks = NumLocks.fetch_add(1, std::memory_order_relaxed); // relaxed because the following
//		// `AddSubsequent` provides required sync
//		assert(PrevNumLocks + 1 < ExecutionFlag);
//
//		if (!Prerequisite.AddSubsequent(*this)) // linearisation point, acq_rel semantic
//		{
//			// failed to add the prerequisite (too late), correct the number
//			NumLocks.fetch_sub(1, std::memory_order_relaxed); // relaxed because the previous `AddSubsequent` call provides required sync
//			return false;
//		}
//
//		Prerequisite.AddRef(); // keep it alive until this task's execution
//		Prerequisites.push(&Prerequisite); // release memory order
//		return true;
//	}
//	template<typename HigherLevelTaskType, decltype(std::declval<HigherLevelTaskType>().Pimpl)* = nullptr>
//	bool AddPrerequisites(const HigherLevelTaskType& Prerequisite)
//	{
//		return AddPrerequisites(*Prerequisite.Pimpl);
//	}
//
//	template<typename PrerequisiteCollectionType, decltype(std::declval<PrerequisiteCollectionType>().begin())* = nullptr>
//	void AddPrerequisites(const PrerequisiteCollectionType& InPrerequisites)
//	{
//		checkf(NumLocks.load(std::memory_order_relaxed) >= NumInitialLocks && NumLocks.load(std::memory_order_relaxed) < ExecutionFlag, TEXT("Prerequisites can be added only before the task is launched"));
//
//		// registering the task as a subsequent of the given prerequisite can cause its immediate launch by the prerequisite
//		// (if the prerequisite has been completed on another thread), so we need to keep the task locked by assuming that the 
//		// prerequisite can be added successfully, and release the lock if it wasn't
//		uint32 PrevNumLocks = NumLocks.fetch_add(GetNum(InPrerequisites), std::memory_order_relaxed); // relaxed because the following
//		// `AddSubsequent` provides required sync
//		checkf(PrevNumLocks + GetNum(InPrerequisites) < ExecutionFlag, TEXT("Max number of nested tasks reached: %d"), ExecutionFlag);
//
//		uint32 NumCompletedPrerequisites = 0;
//		for (auto& Prereq : InPrerequisites)
//		{
//			// prerequisites can be either `FTaskBase*` or its Pimpl handle
//			FTaskBase* Prerequisite;
//			using FPrerequisiteType = std::decay_t<decltype(*std::declval<PrerequisiteCollectionType>().begin())>;
//			if constexpr (std::is_same_v<FPrerequisiteType, FTaskBase*>)
//			{
//				Prerequisite = Prereq;
//			}
//			else if constexpr (std::is_pointer_v<FPrerequisiteType>)
//			{
//				Prerequisite = Prereq->Pimpl;
//			}
//			else
//			{
//				Prerequisite = Prereq.Pimpl;
//			}
//
//			if (Prerequisite->AddSubsequent(*this)) // acq_rel memory order
//			{
//				Prerequisite->AddRef(); // keep it alive until this task's execution
//				Prerequisites.Push(Prerequisite); // release memory order
//			}
//			else
//			{
//				++NumCompletedPrerequisites;
//			}
//		}
//
//		// unlock for prerequisites that weren't added
//		NumLocks.fetch_sub(NumCompletedPrerequisites, std::memory_order_relaxed);  // relaxed because the previous 
//		// `AddSubsequent` provides required sync
//	}
//
//	bool AddSubsequent(TaskBase& Subsequent)
//	{
//		//TaskTrace::SubsequentAdded(GetTraceId(), Subsequent.GetTraceId()); // doesn't matter if we suceeded below, we need to record task dependency
//		return Subsequents.trypush(&Subsequent);
//	}
//
//
//	bool TryLaunch()
//	{
//		return TryUnlock();
//	}
//
//	// @return true if the task was executed and all its nested tasks are completed
//	bool IsCompleted() const
//	{
//		return Subsequents.empty();
//	}
//	
//	void AddNested(TaskBase& Nested)
//	{
//		uint32_t PrevNumLocks = NumLocks.fetch_add(1, std::memory_order_relaxed); // in case we'll succeed in adding subsequent, 
//		// "happens before" registering this task as a subsequent
//		assert(PrevNumLocks + 1 < std::numeric_limits<uint32_t>::max());
//		assert(PrevNumLocks > ExecutionFlag);
//
//		if (Nested.AddSubsequent(*this)) // "release" memory order
//		{
//			Nested.AddRef(); // keep it alive as we store it in `Prerequisites` and we can need it to try to retract it. it's released on closing the task
//			Prerequisites.push(&Nested);
//		}
//		else
//		{
//			NumLocks.fetch_sub(1, std::memory_order_relaxed);
//		}
//	}
//
//	void BusyWait()
//	{
//		if (!TryRetractAndExecute())
//		{
//			LowLevelTasks::BusyWaitUntil([this] { return IsCompleted(); });
//		}
//	}
//
//	// waits until the task is completed or waiting timed out, while executing other tasks
//	bool BusyWait(Timespan InTimeout)
//	{
//
//		Timespan Timeout{ InTimeout };
//
//		if (TryRetractAndExecute())
//		{
//			return true;
//		}
//
//		LowLevelTasks::BusyWaitUntil([this, Timeout] { return IsCompleted() || Timeout; });
//		return IsCompleted();
//	}
//
//	template<typename ConditionType>
//	bool BusyWait(ConditionType&& Condition)
//	{
//		TaskTrace::FWaitingScope WaitingScope(GetTraceId());
//		TRACE_CPUPROFILER_EVENT_SCOPE(Tasks::BusyWait);
//
//		if (TryRetractAndExecute())
//		{
//			return true;
//		}
//
//		LowLevelTasks::BusyWaitUntil(
//			[this, Condition = std::forward<ConditionType>(Condition)]{ return IsCompleted() || Condition(); }
//		);
//		return IsCompleted();
//	}
//
//	protected:
//		using FTaskBodyType = void(*)(TaskBase&);
//
//		// tries to get execution permission and if successful, executes given task body and completes the task if there're no pending nested tasks. 
//		// does all required accounting before/after task execution. the task can be deleted as a result of this call.
//		// @returns true if the task was executed by the current thread
//		__forceinline bool TryExecute(FTaskBodyType TaskBody)
//		{
//			if (!TrySetExecutionFlag())
//			{
//				return false;
//			}
//
//			AddRef(); // `LowLevelTask` will automatically release the internal reference after execution, but there can be pending nested tasks, so keep it alive
//			// it's released either later here if the task is closed, or when the last nested task is completed and unlocks its parent (in `TryUnlock`)
//
//			ReleasePrerequisites();
//
//			TaskBase* PrevTask = ExchangeCurrentTask(this);
//			ExecutingThreadId.store(*static_cast<unsigned int*>(static_cast<void*>(&std::this_thread::get_id())), std::memory_order_relaxed);
//
//			if (GetPipe() != nullptr)
//			{
//				StartPipeExecution();
//			}
//
//			{
//				TaskBody(*this);
//			}
//
//			if (GetPipe() != nullptr)
//			{
//				FinishPipeExecution();
//			}
//
//			ExecutingThreadId.store(INVALID_THREAD_ID, std::memory_order_relaxed); // no need to sync with loads as they matter only if
//			// executed by the same thread
//			ExchangeCurrentTask(PrevTask);
//
//			// close the task if there are no pending nested tasks
//			uint32_t LocalNumLocks = NumLocks.fetch_sub(1, std::memory_order_acq_rel) - 1; // "release" to make task execution "happen before" this, and "acquire" to 
//			// "sync with" another thread that completed the last nested task
//			if (LocalNumLocks == ExecutionFlag) // unlocked (no pending nested tasks)
//			{
//				Close();
//				Release(); // the internal reference that kept the task alive for nested tasks
//			} // else there're non completed nested tasks, the last one will unlock, close and release the parent (this task)
//
//			return true;
//		}
//
//		// closes task by unlocking its subsequents and flagging it as completed
//		void Close()
//		{
//			static_assert(!IsCompleted());
//
//			if (GetPipe() != nullptr)
//			{
//				ClearPipe();
//			}
//
//			std::vector<TaskBase*> Subs;
//			auto entry = Subsequents.PopAll();
//			
//			Subsequents.PopAll(Subs);
//			for (TaskBase* Sub : Subs)
//			{
//				Sub->TryUnlock();
//			}
//
//			// release nested tasks
//			ReleasePrerequisites();
//
//		}
//
//
//	private:
//		// A task can be locked for execution (by prerequisites or if it's not launched yet) or for completion (by nested tasks).
//		// This method is called to unlock the task and so can result in its scheduling (and execution) or completion
//		bool TryUnlock()
//		{
//			FPipe* LocalPipe = GetPipe(); // cache data locally so we won't need to touch the member (read below)
//
//			uint32_t PrevNumLocks = NumLocks.fetch_sub(1, std::memory_order_acq_rel); // `acq_rel` to make it happen after task 
//			// preparation and before launching it
//			// the task can be dead already as the prev line can remove the lock hold for this execution path, another thread(s) can unlock
//			// the task, execute, complete and delete it. thus before touching any members or calling methods we need to make sure
//			// the task can't be destroyed concurrently
//
//			uint32_t LocalNumLocks = PrevNumLocks - 1;
//
//			if (PrevNumLocks < ExecutionFlag)
//			{
//				// pre-execution state, try to schedule the task
//
//				assert(PrevNumLocks != 0);
//
//				bool bPrerequisitesCompleted = LocalPipe == nullptr ? LocalNumLocks == 0 : LocalNumLocks <= 1; // the only remaining lock is pipe's one (if any)
//				if (!bPrerequisitesCompleted)
//				{
//					return false;
//				}
//
//				// this thread unlocked the task, no other thread can reach this point concurrently, we can touch the task again
//
//				if (LocalPipe != nullptr)
//				{
//					bool bFirstPipingAttempt = LocalNumLocks == 1;
//					if (bFirstPipingAttempt)
//					{
//						TaskBase* PrevPipedTask = TryPushIntoPipe();
//						if (PrevPipedTask != nullptr) // the pipe is blocked
//						{
//							// the prev task in pipe's chain becomes this task's prerequisite, to enabled piped task retraction.
//							// no need to AddRef as it's already sorted in `FPipe::PushIntoPipe`
//							Prerequisites.Push(PrevPipedTask);
//							return false;
//						}
//
//						NumLocks.store(0, std::memory_order_release); // release pipe's lock
//					}
//				}
//
//				if (ExtendedPriority == EExtendedTaskPriority::Inline)
//				{
//					// "inline" tasks are not scheduled but executed straight away
//					TryExecuteTask(); // result doesn't matter, this can fail if task retraction jumped in and got execution
//					// permission between this thread unlocked the task and tried to execute it
//					verify(LowLevelTask.TryCancel());
//				}
//				else if (ExtendedPriority == EExtendedTaskPriority::TaskEvent)
//				{
//					// task events have nothing to execute, try to close it. task retraction can jump in and close the task event, 
//					// so this thread still needs to check execution permission
//					if (TrySetExecutionFlag())
//					{
//						// task events are used as an empty prerequisites/subsequents
//						ReleasePrerequisites();
//						Close();
//						verify(LowLevelTask.TryCancel()); // releases the internal reference
//					}
//				}
//				else
//				{
//					Schedule();
//				}
//
//				return true;
//			}
//
//			// execution already started (at least), this is nested tasks unlocking their parent
//			assert(PrevNumLocks != ExecutionFlag);
//			if (LocalNumLocks != ExecutionFlag) // still locked
//			{
//				return false;
//			}
//
//			// this thread unlocked the task, no other thread can reach this point concurrently, we can touch the task again
//			Close();
//			Release(); // the internal reference that kept the task alive for nested tasks
//			return true;
//		}
//
//		bool TrySetExecutionFlag()
//		{
//			uint32_t ExpectedUnlocked = 0;
//			// set the execution flag and simultenously lock it (+1) so a nested task completion doesn't close it before its execution is finished
//			return NumLocks.compare_exchange_strong(ExpectedUnlocked, ExecutionFlag + 1, std::memory_order_acq_rel, std::memory_order_relaxed); // on success 
//			// - linearisation point for task execution, on failure - load order doesn't matter
//		}
//
//		void ReleasePrerequisites()
//		{
//			TaskBase* Prerequisite;
//			Prerequisites.pop(Prerequisite);
//			while (Prerequisite)
//			{
//				Prerequisite->Release();
//				Prerequisites.pop(Prerequisite);
//			}
//		}
//
//		private:
//			EExtendedTaskPriority ExtendedPriority; // internal priorities, if any
//
//			FTask LowLevelTask;
//
//			// the number of times that the task should be unlocked before it can be scheduled or completed
//			// initial count is 1 for launching the task (it can't be scheduled before it's launched)
//			// reaches 0 the task is scheduled for execution.
//			// NumLocks's the most significant bit (see `ExecutionFlag`) is set on task execution start, and indicates that now 
//			// NumLocks is about how many times the task must be unlocked to be completed
//			static constexpr uint32_t NumInitialLocks = 1;
//			std::atomic<uint32_t> NumLocks{ NumInitialLocks };
//
//			MPSCQueue<TaskBase*> Subsequents{0};
//
//			// stores backlinks to prerequsites, either execution prerequisites or nested tasks (completion prerequisites).
//			// It's populated in three stages:
//			// 1) by adding execution prerequisites, before the task is launched.
//			// 2) by piping, when the previous piped task (if any) is added as a prerequisite. can happen concurrently with other threads accessing prerequisites for
//			//		task retraction.
//			// 3) by adding nested tasks. after piping. during task execution.
//			rigtorp::MPMCQueue<TaskBase*> Prerequisites{0};
//
//			FPipe* Pipe{ nullptr };
//
//			std::atomic<uint32_t> ExecutingThreadId = INVALID_THREAD_ID;
//};
//
//class TaskEventBase : public TaskBase
//{
//public:
//	static TaskEventBase* Create(const char* DebugName)
//	{
//		return new TaskEventBase(DebugName);
//	}
//
//	static void* operator new(size_t Size);
//	static void operator delete(void* Ptr);
//
//private:
//	TaskEventBase(const char* InDebugName)
//		: TaskBase(/*InitRefCount=*/ 1) // for the initial reference (we don't increment it on passing to `TRefCountPtr`)
//	{
//		Init(InDebugName, ETaskPriority::Normal, EExtendedTaskPriority::TaskEvent);
//	}
//
//	virtual bool TryExecuteTask() override
//	{
//		//checkNoEntry(); // never executed because it doesn't have a task body
//		return true;
//	}
//};
//class FTaskHandle
//{
//	// friends to get access to `Pimpl`
//	friend TaskBase;
//
//	template<typename... TaskTypes>
//	friend class TPrerequisites;
//
//	template<typename TaskCollectionType>
//	friend bool TryRetractAndExecute(const TaskCollectionType& Tasks);
//
//	template<typename TaskCollectionType>
//	friend bool TryRetractAndExecute(const TaskCollectionType& Tasks, Timespan Timeout);
//
//	template<typename TaskCollectionType>
//	friend void Wait(const TaskCollectionType& Tasks);
//
//	template<typename TaskCollectionType>
//	friend bool Wait(const TaskCollectionType& Tasks, Timespan InTimeout);
//
//	template<typename TaskCollectionType>
//	friend bool BusyWait(const TaskCollectionType& Tasks, Timespan InTimeout);
//
//	template<typename TaskType>
//	friend void AddNested(const TaskType& Nested);
//
//protected:
//	explicit FTaskHandle(TaskBase* Other)
//	{
//		Pimpl = std::make_shared<TaskBase*>(Other);
//	}
//
//public:
//	FTaskHandle() = default;
//
//	bool IsValid() const
//	{
//		return Pimpl.get() == nullptr;
//	}
//
//	// checks if task's execution is done
//	bool IsCompleted() const
//	{
//		return !IsValid() || Pimpl->IsCompleted();
//	}
//
//	// waits for task's completion. Tries to retract the task and execute it in-place, if failed - blocks until the task 
//	// is completed by another thread. If timeout is zero, tries to retract the task and returns immedially after that.
//	// @return true if the task is completed
//	void Wait()
//	{
//		if (IsValid())
//		{
//			Pimpl->Wait();
//		}
//	}
//
//	// waits for task's completion with timeout. Tries to retract the task and execute it in-place, if failed - blocks until the task 
//	// is completed by another thread. If timeout is zero, tries to retract the task and returns immedially after that.
//	// @return true if the task is completed
//	bool Wait(Timespan Timeout)
//	{
//		return !IsValid() || Pimpl->Wait(Timeout);
//	}
//
//	// waits for task's completion while executing other tasks. Shouldn't be used inside a latency-sensitive task
//	void BusyWait()
//	{
//		if (IsValid())
//		{
//			Pimpl->BusyWait();
//		}
//	}
//
//	// waits for task's completion for at least the specified amount of time, while executing other tasks.
//	// the call can return much later than the given timeout
//	// @return true if the task is completed
//	bool BusyWait(Timespan Timeout)
//	{
//		return !IsValid() || Pimpl->BusyWait(Timeout);
//	}
//
//	// waits for task's completion or the given condition becomes true, while executing other tasks.
//	// the call can return much later than the given condition became true
//	// @return true if the task is completed
//	template<typename ConditionType>
//	bool BusyWait(ConditionType&& Condition)
//	{
//		return !IsValid() || Pimpl->BusyWait(Forward<ConditionType>(Condition));
//	}
//
//	// launches a task for asynchronous execution
//	// @param DebugName - a unique name for task identification in debugger and profiler, is compiled out in test/shipping builds
//	// @param TaskBody - a functor that will be executed asynchronously
//	// @param Priority - task priority that affects when the task will be executed
//	// @return a trivially relocatable instance that can be used to wait for task completion or to obtain task execution result
//	template<typename TaskBodyType>
//	void Launch(
//		const char* DebugName,
//		TaskBodyType&& TaskBody,
//		ETaskPriority Priority = ETaskPriority::Normal,
//		EExtendedTaskPriority ExtendedPriority = EExtendedTaskPriority::None
//	)
//	{
//		assert(!IsValid());
//
//		using FExecutableTask = Private::TExecutableTask<std::decay_t<TaskBodyType>>;
//		FExecutableTask* Task = FExecutableTask::Create(DebugName, Forward<TaskBodyType>(TaskBody), Priority, ExtendedPriority);
//		// this must happen before launching, to support an ability to access the task itself from inside it
//		*Pimpl.GetInitReference() = Task;
//		Task->TryLaunch();
//	}
//
//	// launches a task for asynchronous execution, with prerequisites that must be completed before the task is scheduled
//	// @param DebugName - a unique name for task identification in debugger and profiler, is compiled out in test/shipping builds
//	// @param TaskBody - a functor that will be executed asynchronously
//	// @param Prerequisites - tasks or task events that must be completed before the task being launched can be scheduled, accepts any 
//	// iterable collection (.begin()/.end()), `Tasks::Prerequisites()` helper is recommended to create such collection on the fly
//	// @param Priority - task priority that affects when the task will be executed
//	// @return a trivially relocatable instance that can be used to wait for task completion or to obtain task execution result
//	template<typename TaskBodyType, typename PrerequisitesCollectionType>
//	void Launch(
//		const char* DebugName,
//		TaskBodyType&& TaskBody,
//		PrerequisitesCollectionType&& Prerequisites,
//		ETaskPriority Priority = ETaskPriority::Normal,
//		EExtendedTaskPriority ExtendedPriority = EExtendedTaskPriority::None
//	)
//	{
//		check(!IsValid());
//
//		using FExecutableTask = Private::TExecutableTask<std::decay_t<TaskBodyType>>;
//		FExecutableTask* Task = FExecutableTask::Create(DebugName, Forward<TaskBodyType>(TaskBody), Priority, ExtendedPriority);
//		Task->AddPrerequisites(Forward<PrerequisitesCollectionType>(Prerequisites));
//		// this must happen before launching, to support an ability to access the task itself from inside it
//		*Pimpl.GetInitReference() = Task;
//		Task->TryLaunch();
//	}
//
//	bool IsAwaitable() const
//	{
//		return IsValid() && Pimpl->IsAwaitable();
//	}
//
//	// Creates and returns a "task event" that can be used to wait for the task completion.
//	// Regular tasks are allocated by a fast allocator that doesn't handle well long-living tasks, as such tasks can keep an entire memory page alive, 
//	// thus pushing up total memory consumption. In most cases such tasks are stored as class members, or as global vars.
//	// hovewer, task events use a different allocator and doesn't cause this issue, and so can be used for long-living tasks.
//	// Make sure to profile and identify that you indeed have "long-living task" problem before using this function, 
//	// as otherwise it would be a needless overhead.
//	// Task events don't support execution result. if you do need execution result, most probably you can reset your task right after getting its execution
//	// result, so it's not long-living anymore. 
//	FTaskHandle CreateCompletionHandle()
//	{
//		if (!IsValid() || IsCompleted())
//		{
//			return {};
//		}
//
//		// `FTaskEventBase` uses an allocator that doesn't have an issue with long-living allocs
//		Private::FTaskEventBase* CompletionHandle{ Private::FTaskEventBase::Create(TEXT("CompletionHandle")) };
//		if (!CompletionHandle->AddPrerequisites(*Pimpl))
//		{
//			delete CompletionHandle;
//			return {}; // too late, the task is already completed
//		}
//
//		CompletionHandle->AddRef(); // internal reference that is released when the handle is completed
//		// trigger the completion handle so the only thing that holds it from signalling is the task itself
//		CompletionHandle->TryLaunch();
//		return FTaskHandle{ CompletionHandle };
//	}
//
//protected:
//	std::shared_ptr<TaskBase> Pimpl;
//};

#endif // !TASK_H
