/*
 * Copyright (c) 2000-2005 The Regents of The University of Michigan
 * Copyright (c) 2013 Advanced Micro Devices, Inc.
 * Copyright (c) 2013 Mark D. Hill and David A. Wood
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Authors: Steve Reinhardt
 *          Nathan Binkert
 */

/* @file
 * EventQueue interfaces
 */

#ifndef __SIM_EVENTQ_HH__
#define __SIM_EVENTQ_HH__

#include <algorithm>
#include <cassert>
#include <climits>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <string>

#include "base/flags.hh"
#include "base/types.hh"
#include "debug/Event.hh"
#include "sim/serialize.hh"

class EventQueue;       // forward declaration
class BaseGlobalEvent;

//! Simulation Quantum for multiple eventq simulation.
//! The quantum value is the period length after which the queues
//! synchronize themselves with each other. This means that any
//! event to scheduled on Queue A which is generated by an event on
//! Queue B should be at least simQuantum ticks away in future.
extern Tick simQuantum;

//! Current number of allocated main event queues.
extern uint32_t numMainEventQueues;

//! Array for main event queues.
extern std::vector<EventQueue *> mainEventQueue;

//! The current event queue for the running thread. Access to this queue
//! does not require any locking from the thread.

extern __thread EventQueue *_curEventQueue;

//! Current mode of execution: parallel / serial
extern bool inParallelMode;

//! Function for returning eventq queue for the provided
//! index. The function allocates a new queue in case one
//! does not exist for the index, provided that the index
//! is with in bounds.
EventQueue *getEventQueue(uint32_t index);

inline EventQueue *curEventQueue() { return _curEventQueue; }
inline void curEventQueue(EventQueue *q) { _curEventQueue = q; }

/**
 * Common base class for Event and GlobalEvent, so they can share flag
 * and priority definitions and accessor functions.  This class should
 * not be used directly.
 */
class EventBase
{
  protected:
    typedef unsigned short FlagsType;
    typedef ::Flags<FlagsType> Flags;

    static const FlagsType PublicRead    = 0x003f; // public readable flags
    static const FlagsType PublicWrite   = 0x001d; // public writable flags
    static const FlagsType Squashed      = 0x0001; // has been squashed
    static const FlagsType Scheduled     = 0x0002; // has been scheduled
    static const FlagsType AutoDelete    = 0x0004; // delete after dispatch
    /**
     * This used to be AutoSerialize. This value can't be reused
     * without changing the checkpoint version since the flag field
     * gets serialized.
     */
    static const FlagsType Reserved0     = 0x0008;
    static const FlagsType IsExitEvent   = 0x0010; // special exit event
    static const FlagsType IsMainQueue   = 0x0020; // on main event queue
    static const FlagsType Initialized   = 0x7a40; // somewhat random bits
    static const FlagsType InitMask      = 0xffc0; // mask for init bits

  public:
    typedef int8_t Priority;

    /// Event priorities, to provide tie-breakers for events scheduled
    /// at the same cycle.  Most events are scheduled at the default
    /// priority; these values are used to control events that need to
    /// be ordered within a cycle.

    /// Minimum priority
    static const Priority Minimum_Pri =          SCHAR_MIN;

    /// If we enable tracing on a particular cycle, do that as the
    /// very first thing so we don't miss any of the events on
    /// that cycle (even if we enter the debugger).
    static const Priority Debug_Enable_Pri =          -101;

    /// Breakpoints should happen before anything else (except
    /// enabling trace output), so we don't miss any action when
    /// debugging.
    static const Priority Debug_Break_Pri =           -100;

    /// CPU switches schedule the new CPU's tick event for the
    /// same cycle (after unscheduling the old CPU's tick event).
    /// The switch needs to come before any tick events to make
    /// sure we don't tick both CPUs in the same cycle.
    static const Priority CPU_Switch_Pri =             -31;

    /// For some reason "delayed" inter-cluster writebacks are
    /// scheduled before regular writebacks (which have default
    /// priority).  Steve?
    static const Priority Delayed_Writeback_Pri =       -1;

    /// Default is zero for historical reasons.
    static const Priority Default_Pri =                  0;

    /// DVFS update event leads to stats dump therefore given a lower priority
    /// to ensure all relevant states have been updated
    static const Priority DVFS_Update_Pri =             31;

    /// Serailization needs to occur before tick events also, so
    /// that a serialize/unserialize is identical to an on-line
    /// CPU switch.
    static const Priority Serialize_Pri =               32;

    /// CPU ticks must come after other associated CPU events
    /// (such as writebacks).
    static const Priority CPU_Tick_Pri =                50;

    /// Statistics events (dump, reset, etc.) come after
    /// everything else, but before exit.
    static const Priority Stat_Event_Pri =              90;

    /// Progress events come at the end.
    static const Priority Progress_Event_Pri =          95;

    /// If we want to exit on this cycle, it's the very last thing
    /// we do.
    static const Priority Sim_Exit_Pri =               100;

    /// Maximum priority
    static const Priority Maximum_Pri =          SCHAR_MAX;
};

/*
 * An item on an event queue.  The action caused by a given
 * event is specified by deriving a subclass and overriding the
 * process() member function.
 *
 * Caution, the order of members is chosen to maximize data packing.
 */
class Event : public EventBase, public Serializable
{
    friend class EventQueue;

  private:
    // The event queue is now a linked list of linked lists.  The
    // 'nextBin' pointer is to find the bin, where a bin is defined as
    // when+priority.  All events in the same bin will be stored in a
    // second linked list (a stack) maintained by the 'nextInBin'
    // pointer.  The list will be accessed in LIFO order.  The end
    // result is that the insert/removal in 'nextBin' is
    // linear/constant, and the lookup/removal in 'nextInBin' is
    // constant/constant.  Hopefully this is a significant improvement
    // over the current fully linear insertion.
    Event *nextBin;
    Event *nextInBin;

    static Event *insertBefore(Event *event, Event *curr);
    static Event *removeItem(Event *event, Event *last);

    Tick _when;         //!< timestamp when event should be processed
    Priority _priority; //!< event priority
    Flags flags;

#ifndef NDEBUG
    /// Global counter to generate unique IDs for Event instances
    static Counter instanceCounter;

    /// This event's unique ID.  We can also use pointer values for
    /// this but they're not consistent across runs making debugging
    /// more difficult.  Thus we use a global counter value when
    /// debugging.
    Counter instance;

    /// queue to which this event belongs (though it may or may not be
    /// scheduled on this queue yet)
    EventQueue *queue;
#endif

#ifdef EVENTQ_DEBUG
    Tick whenCreated;   //!< time created
    Tick whenScheduled; //!< time scheduled
#endif

    void
    setWhen(Tick when, EventQueue *q)
    {
        _when = when;
#ifndef NDEBUG
        queue = q;
#endif
#ifdef EVENTQ_DEBUG
        whenScheduled = curTick();
#endif
    }

    bool
    initialized() const
    {
        return (flags & InitMask) == Initialized;
    }

  protected:
    /// Accessor for flags.
    Flags
    getFlags() const
    {
        return flags & PublicRead;
    }

    bool
    isFlagSet(Flags _flags) const
    {
        assert(_flags.noneSet(~PublicRead));
        return flags.isSet(_flags);
    }

    /// Accessor for flags.
    void
    setFlags(Flags _flags)
    {
        assert(_flags.noneSet(~PublicWrite));
        flags.set(_flags);
    }

    void
    clearFlags(Flags _flags)
    {
        assert(_flags.noneSet(~PublicWrite));
        flags.clear(_flags);
    }

    void
    clearFlags()
    {
        flags.clear(PublicWrite);
    }

    // This function isn't really useful if TRACING_ON is not defined
    virtual void trace(const char *action);     //!< trace event activity

  public:

    /*
     * Event constructor
     * @param queue that the event gets scheduled on
     */
    Event(Priority p = Default_Pri, Flags f = 0)
        : nextBin(nullptr), nextInBin(nullptr), _when(0), _priority(p),
          flags(Initialized | f)
    {
        assert(f.noneSet(~PublicWrite));
#ifndef NDEBUG
        instance = ++instanceCounter;
        queue = NULL;
#endif
#ifdef EVENTQ_DEBUG
        whenCreated = curTick();
        whenScheduled = 0;
#endif
    }

    virtual ~Event();
    virtual const std::string name() const;

    /// Return a C string describing the event.  This string should
    /// *not* be dynamically allocated; just a const char array
    /// describing the event class.
    virtual const char *description() const;

    /// Dump the current event data
    void dump() const;

  public:
    /*
     * This member function is invoked when the event is processed
     * (occurs).  There is no default implementation; each subclass
     * must provide its own implementation.  The event is not
     * automatically deleted after it is processed (to allow for
     * statically allocated event objects).
     *
     * If the AutoDestroy flag is set, the object is deleted once it
     * is processed.
     */
    virtual void process() = 0;

    /// Determine if the current event is scheduled
    bool scheduled() const { return flags.isSet(Scheduled); }

    /// Squash the current event
    void squash() { flags.set(Squashed); }

    /// Check whether the event is squashed
    bool squashed() const { return flags.isSet(Squashed); }

    /// See if this is a SimExitEvent (without resorting to RTTI)
    bool isExitEvent() const { return flags.isSet(IsExitEvent); }

    /// Check whether this event will auto-delete
    bool isAutoDelete() const { return flags.isSet(AutoDelete); }

    /// Get the time that the event is scheduled
    Tick when() const { return _when; }

    /// Get the event priority
    Priority priority() const { return _priority; }

    //! If this is part of a GlobalEvent, return the pointer to the
    //! Global Event.  By default, there is no GlobalEvent, so return
    //! NULL.  (Overridden in GlobalEvent::BarrierEvent.)
    virtual BaseGlobalEvent *globalEvent() { return NULL; }

    void serialize(CheckpointOut &cp) const override;
    void unserialize(CheckpointIn &cp) override;
};

inline bool
operator<(const Event &l, const Event &r)
{
    return l.when() < r.when() ||
        (l.when() == r.when() && l.priority() < r.priority());
}

inline bool
operator>(const Event &l, const Event &r)
{
    return l.when() > r.when() ||
        (l.when() == r.when() && l.priority() > r.priority());
}

inline bool
operator<=(const Event &l, const Event &r)
{
    return l.when() < r.when() ||
        (l.when() == r.when() && l.priority() <= r.priority());
}
inline bool
operator>=(const Event &l, const Event &r)
{
    return l.when() > r.when() ||
        (l.when() == r.when() && l.priority() >= r.priority());
}

inline bool
operator==(const Event &l, const Event &r)
{
    return l.when() == r.when() && l.priority() == r.priority();
}

inline bool
operator!=(const Event &l, const Event &r)
{
    return l.when() != r.when() || l.priority() != r.priority();
}

/**
 * Queue of events sorted in time order
 *
 * Events are scheduled (inserted into the event queue) using the
 * schedule() method. This method either inserts a <i>synchronous</i>
 * or <i>asynchronous</i> event.
 *
 * Synchronous events are scheduled using schedule() method with the
 * argument 'global' set to false (default). This should only be done
 * from a thread holding the event queue lock
 * (EventQueue::service_mutex). The lock is always held when an event
 * handler is called, it can therefore always insert events into its
 * own event queue unless it voluntarily releases the lock.
 *
 * Events can be scheduled across thread (and event queue borders) by
 * either scheduling asynchronous events or taking the target event
 * queue's lock. However, the lock should <i>never</i> be taken
 * directly since this is likely to cause deadlocks. Instead, code
 * that needs to schedule events in other event queues should
 * temporarily release its own queue and lock the new queue. This
 * prevents deadlocks since a single thread never owns more than one
 * event queue lock. This functionality is provided by the
 * ScopedMigration helper class. Note that temporarily migrating
 * between event queues can make the simulation non-deterministic, it
 * should therefore be limited to cases where that can be tolerated
 * (e.g., handling asynchronous IO or fast-forwarding in KVM).
 *
 * Asynchronous events can also be scheduled using the normal
 * schedule() method with the 'global' parameter set to true. Unlike
 * the previous queue migration strategy, this strategy is fully
 * deterministic. This causes the event to be inserted in a separate
 * queue of asynchronous events (async_queue), which is merged main
 * event queue at the end of each simulation quantum (by calling the
 * handleAsyncInsertions() method). Note that this implies that such
 * events must happen at least one simulation quantum into the future,
 * otherwise they risk being scheduled in the past by
 * handleAsyncInsertions().
 */
class EventQueue
{
  private:
    std::string objName;
    Event *head;
    Tick _curTick;

    //! Mutex to protect async queue.
    std::mutex async_queue_mutex;

    //! List of events added by other threads to this event queue.
    std::list<Event*> async_queue;

    /**
     * Lock protecting event handling.
     *
     * This lock is always taken when servicing events. It is assumed
     * that the thread scheduling new events (not asynchronous events
     * though) have taken this lock. This is normally done by
     * serviceOne() since new events are typically scheduled as a
     * response to an earlier event.
     *
     * This lock is intended to be used to temporarily steal an event
     * queue to support inter-thread communication when some
     * deterministic timing can be sacrificed for speed. For example,
     * the KVM CPU can use this support to access devices running in a
     * different thread.
     *
     * @see EventQueue::ScopedMigration.
     * @see EventQueue::ScopedRelease
     * @see EventQueue::lock()
     * @see EventQueue::unlock()
     */
    std::mutex service_mutex;

    //! Insert / remove event from the queue. Should only be called
    //! by thread operating this queue.
    void insert(Event *event);
    void remove(Event *event);

    //! Function for adding events to the async queue. The added events
    //! are added to main event queue later. Threads, other than the
    //! owning thread, should call this function instead of insert().
    void asyncInsert(Event *event);

    EventQueue(const EventQueue &);

  public:
    /**
     * Temporarily migrate execution to a different event queue.
     *
     * An instance of this class temporarily migrates execution to a
     * different event queue by releasing the current queue, locking
     * the new queue, and updating curEventQueue(). This can, for
     * example, be useful when performing IO across thread event
     * queues when timing is not crucial (e.g., during fast
     * forwarding).
     */
    class ScopedMigration
    {
      public:
        ScopedMigration(EventQueue *_new_eq)
            :  new_eq(*_new_eq), old_eq(*curEventQueue())
        {
            old_eq.unlock();
            new_eq.lock();
            curEventQueue(&new_eq);
        }

        ~ScopedMigration()
        {
            new_eq.unlock();
            old_eq.lock();
            curEventQueue(&old_eq);
        }

      private:
        EventQueue &new_eq;
        EventQueue &old_eq;
    };

    /**
     * Temporarily release the event queue service lock.
     *
     * There are cases where it is desirable to temporarily release
     * the event queue lock to prevent deadlocks. For example, when
     * waiting on the global barrier, we need to release the lock to
     * prevent deadlocks from happening when another thread tries to
     * temporarily take over the event queue waiting on the barrier.
     */
    class ScopedRelease
    {
      public:
        ScopedRelease(EventQueue *_eq)
            :  eq(*_eq)
        {
            eq.unlock();
        }

        ~ScopedRelease()
        {
            eq.lock();
        }

      private:
        EventQueue &eq;
    };

    EventQueue(const std::string &n);

    virtual const std::string name() const { return objName; }
    void name(const std::string &st) { objName = st; }

    //! Schedule the given event on this queue. Safe to call from any
    //! thread.
    void schedule(Event *event, Tick when, bool global = false);

    //! Deschedule the specified event. Should be called only from the
    //! owning thread.
    void deschedule(Event *event);

    //! Reschedule the specified event. Should be called only from
    //! the owning thread.
    void reschedule(Event *event, Tick when, bool always = false);

    Tick nextTick() const { return head->when(); }
    void setCurTick(Tick newVal) { _curTick = newVal; }
    Tick getCurTick() const { return _curTick; }
    Event *getHead() const { return head; }

    Event *serviceOne();

    // process all events up to the given timestamp.  we inline a
    // quick test to see if there are any events to process; if so,
    // call the internal out-of-line version to process them all.
    void
    serviceEvents(Tick when)
    {
        while (!empty()) {
            if (nextTick() > when)
                break;

            /**
             * @todo this assert is a good bug catcher.  I need to
             * make it true again.
             */
            //assert(head->when() >= when && "event scheduled in the past");
            serviceOne();
        }

        setCurTick(when);
    }

    // return true if no events are queued
    bool empty() const { return head == NULL; }

    void dump() const;

    bool debugVerify() const;

    //! Function for moving events from the async_queue to the main queue.
    void handleAsyncInsertions();

    /**
     *  Function to signal that the event loop should be woken up because
     *  an event has been scheduled by an agent outside the gem5 event
     *  loop(s) whose event insertion may not have been noticed by gem5.
     *  This function isn't needed by the usual gem5 event loop but may
     *  be necessary in derived EventQueues which host gem5 onto other
     *  schedulers.
     *
     *  @param when Time of a delayed wakeup (if known). This parameter
     *  can be used by an implementation to schedule a wakeup in the
     *  future if it is sure it will remain active until then.
     *  Or it can be ignored and the event queue can be woken up now.
     */
    virtual void wakeup(Tick when = (Tick)-1) { }

    /**
     *  function for replacing the head of the event queue, so that a
     *  different set of events can run without disturbing events that have
     *  already been scheduled. Already scheduled events can be processed
     *  by replacing the original head back.
     *  USING THIS FUNCTION CAN BE DANGEROUS TO THE HEALTH OF THE SIMULATOR.
     *  NOT RECOMMENDED FOR USE.
     */
    Event* replaceHead(Event* s);

    /**@{*/
    /**
     * Provide an interface for locking/unlocking the event queue.
     *
     * @warn Do NOT use these methods directly unless you really know
     * what you are doing. Incorrect use can easily lead to simulator
     * deadlocks.
     *
     * @see EventQueue::ScopedMigration.
     * @see EventQueue::ScopedRelease
     * @see EventQueue
     */
    void lock() { service_mutex.lock(); }
    void unlock() { service_mutex.unlock(); }
    /**@}*/

    /**
     * Reschedule an event after a checkpoint.
     *
     * Since events don't know which event queue they belong to,
     * parent objects need to reschedule events themselves. This
     * method conditionally schedules an event that has the Scheduled
     * flag set. It should be called by parent objects after
     * unserializing an object.
     *
     * @warn Only use this method after unserializing an Event.
     */
    void checkpointReschedule(Event *event);

    virtual ~EventQueue() { }
};

void dumpMainQueue();

class EventManager
{
  protected:
    /** A pointer to this object's event queue */
    EventQueue *eventq;

  public:
    EventManager(EventManager &em) : eventq(em.eventq) {}
    EventManager(EventManager *em) : eventq(em->eventq) {}
    EventManager(EventQueue *eq) : eventq(eq) {}

    EventQueue *
    eventQueue() const
    {
        return eventq;
    }

    void
    schedule(Event &event, Tick when)
    {
        eventq->schedule(&event, when);
    }

    void
    deschedule(Event &event)
    {
        eventq->deschedule(&event);
    }

    void
    reschedule(Event &event, Tick when, bool always = false)
    {
        eventq->reschedule(&event, when, always);
    }

    void
    schedule(Event *event, Tick when)
    {
        eventq->schedule(event, when);
    }

    void
    deschedule(Event *event)
    {
        eventq->deschedule(event);
    }

    void
    reschedule(Event *event, Tick when, bool always = false)
    {
        eventq->reschedule(event, when, always);
    }

    void wakeupEventQueue(Tick when = (Tick)-1)
    {
        eventq->wakeup(when);
    }

    void setCurTick(Tick newVal) { eventq->setCurTick(newVal); }
};

template <class T, void (T::* F)()>
void
DelayFunction(EventQueue *eventq, Tick when, T *object)
{
    class DelayEvent : public Event
    {
      private:
        T *object;

      public:
        DelayEvent(T *o)
            : Event(Default_Pri, AutoDelete), object(o)
        { }
        void process() { (object->*F)(); }
        const char *description() const { return "delay"; }
    };

    eventq->schedule(new DelayEvent(object), when);
}

template <class T, void (T::* F)()>
class EventWrapper : public Event
{
  private:
    T *object;

  public:
    EventWrapper(T *obj, bool del = false, Priority p = Default_Pri)
        : Event(p), object(obj)
    {
        if (del)
            setFlags(AutoDelete);
    }

    EventWrapper(T &obj, bool del = false, Priority p = Default_Pri)
        : Event(p), object(&obj)
    {
        if (del)
            setFlags(AutoDelete);
    }

    void process() { (object->*F)(); }

    const std::string
    name() const
    {
        return object->name() + ".wrapped_event";
    }

    const char *description() const { return "EventWrapped"; }
};

#endif // __SIM_EVENTQ_HH__
