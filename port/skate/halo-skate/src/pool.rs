//! The worker pool the host's bots' skate sessions step on (lib.rs).
//!
//! This machine's player's session (0) has a thread of its own, as before.
//! The others - one for each bot on a board, as many as a game has players -
//! share a few threads, as many as the machine has processors (at most
//! `MOST_THREADS`): a thread with the engine's large stack for each session
//! would run a 32-bit game out of address space long before 128 of them.
//! Each session's jobs run in the order they were sent, one at a time; the
//! sessions' jobs run side by side on the pool's threads.

use crate::{Job, Worker, WORKER_STACK};
use std::collections::VecDeque;
use std::sync::{Arc, Condvar, Mutex, OnceLock};

/// The most threads the pool has, whatever the machine.
const MOST_THREADS: usize = 16;

/// A pooled session's jobs not yet done, and its worker (taken by the
/// thread doing its jobs).
struct Queue {
    jobs: VecDeque<Job>,
    worker: Option<Worker>,
    /// On the pool's list of sessions with jobs, or being worked on.
    scheduled: bool,
    /// The session ended (closed, or its worker done): its jobs are dropped.
    closed: bool,
}

/// A session whose jobs run on the pool.
pub(crate) struct Pooled {
    session: usize,
    queue: Mutex<Queue>,
}

struct Pool {
    ready: Mutex<VecDeque<Arc<Pooled>>>,
    wake: Condvar,
}

static POOL: OnceLock<Pool> = OnceLock::new();

/// The pool's threads: as many as the machine has processors, within
/// MOST_THREADS (and at least 1).
pub(crate) fn threads() -> usize {
    std::thread::available_parallelism().map_or(2, |n| n.get()).clamp(1, MOST_THREADS)
}

fn pool() -> &'static Pool {
    POOL.get_or_init(|| {
        let count = threads();
        for index in 0..count {
            let spawned = std::thread::Builder::new()
                .name(format!("skate pool {index}"))
                .stack_size(WORKER_STACK)
                .spawn(run);
            if let Err(e) = spawned {
                crate::log_line(&format!("halo-skate: a pool thread could not start: {e}"));
            }
        }
        crate::log_line(&format!("halo-skate: {count} threads step the bots' skate sessions"));
        Pool { ready: Mutex::new(VecDeque::new()), wake: Condvar::new() }
    })
}

impl Pooled {
    pub(crate) fn new(session: usize, worker: Worker) -> Arc<Self> {
        Arc::new(Self {
            session,
            queue: Mutex::new(Queue { jobs: VecDeque::new(), worker: Some(worker), scheduled: false, closed: false }),
        })
    }

    /// Queues a job; Err when the session ended.
    pub(crate) fn send(self: &Arc<Self>, job: Job) -> Result<(), ()> {
        let schedule = {
            let mut queue = self.queue.lock().unwrap_or_else(|e| e.into_inner());
            if queue.closed {
                return Err(());
            }
            queue.jobs.push_back(job);
            !std::mem::replace(&mut queue.scheduled, true)
        };
        if schedule {
            let pool = pool();
            pool.ready.lock().unwrap_or_else(|e| e.into_inner()).push_back(Arc::clone(self));
            pool.wake.notify_one();
        }
        Ok(())
    }

    /// The session ends: its jobs are dropped, and its worker (its session,
    /// its memory) goes now, or once the job under way is done.
    pub(crate) fn close(&self) {
        let worker = {
            let mut queue = self.queue.lock().unwrap_or_else(|e| e.into_inner());
            queue.closed = true;
            queue.jobs.clear();
            queue.worker.take()
        };
        drop(worker);
    }
}

/// A pool thread: takes a session with jobs, does them all (a few at most,
/// then it goes to the back, so that no session holds a thread), and takes the
/// next.
fn run() {
    let pool = loop {
        if let Some(pool) = POOL.get() {
            break pool;
        }
        std::thread::yield_now();
    };
    loop {
        let pooled = {
            let mut ready = pool.ready.lock().unwrap_or_else(|e| e.into_inner());
            loop {
                if let Some(pooled) = ready.pop_front() {
                    break pooled;
                }
                ready = pool.wake.wait(ready).unwrap_or_else(|e| e.into_inner());
            }
        };
        crate::SESSION.with(|s| s.set(pooled.session));
        let mut worker = pooled.queue.lock().unwrap_or_else(|e| e.into_inner()).worker.take();
        let mut done = 0;
        loop {
            let job = {
                let mut queue = pooled.queue.lock().unwrap_or_else(|e| e.into_inner());
                if queue.closed {
                    queue.scheduled = false;
                    break;
                }
                match queue.jobs.pop_front() {
                    Some(job) if done < 8 => job,
                    Some(job) => {
                        // (a turn's worth: to the back of the list)
                        queue.jobs.push_front(job);
                        queue.worker = worker.take();
                        drop(queue);
                        pool.ready.lock().unwrap_or_else(|e| e.into_inner()).push_back(Arc::clone(&pooled));
                        pool.wake.notify_one();
                        break;
                    }
                    None => {
                        queue.scheduled = false;
                        queue.worker = worker.take();
                        break;
                    }
                }
            };
            done += 1;
            let alive = worker.as_mut().is_some_and(|w| w.handle(job));
            if !alive {
                // (its worker done: the session ends, as a worker thread
                // that returns does; the game sees its replies stop)
                let mut queue = pooled.queue.lock().unwrap_or_else(|e| e.into_inner());
                queue.closed = true;
                queue.jobs.clear();
                queue.scheduled = false;
                worker = None;
                break;
            }
        }
        drop(worker);
        crate::SESSION.with(|s| s.set(0));
    }
}
