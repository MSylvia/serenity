#include "types.h"
#include "Task.h"
#include "kmalloc.h"
#include "VGA.h"
#include "StdLib.h"
#include "i386.h"
#include "system.h"
#include <VirtualFileSystem/FileHandle.h>
#include <VirtualFileSystem/VirtualFileSystem.h>
#include "MemoryManager.h"

Task* current;
Task* s_kernelTask;

static pid_t next_pid;
static InlineLinkedList<Task>* s_tasks;

static bool contextSwitch(Task*);

static void redoKernelTaskTSS()
{
    if (!s_kernelTask->selector())
        s_kernelTask->setSelector(allocateGDTEntry());

    auto& tssDescriptor = getGDTEntry(s_kernelTask->selector());

    tssDescriptor.setBase(&s_kernelTask->tss());
    tssDescriptor.setLimit(0xffff);
    tssDescriptor.dpl = 0;
    tssDescriptor.segment_present = 1;
    tssDescriptor.granularity = 1;
    tssDescriptor.zero = 0;
    tssDescriptor.operation_size = 1;
    tssDescriptor.descriptor_type = 0;
    tssDescriptor.type = 9;

    flushGDT();
}

void Task::prepForIRETToNewTask()
{
    redoKernelTaskTSS();
    s_kernelTask->tss().backlink = current->selector();
    loadTaskRegister(s_kernelTask->selector());
}

void Task::initialize()
{
    current = nullptr;
    next_pid = 0;
    s_tasks = new InlineLinkedList<Task>;
    s_kernelTask = new Task(0, "colonel", IPC::Handle::Any, Task::Ring0);
    redoKernelTaskTSS();
    loadTaskRegister(s_kernelTask->selector());
}

#ifdef TASK_SANITY_CHECKS
void Task::checkSanity(const char* msg)
{
    char ch = current->name()[0];
    kprintf("<%p> %s{%u}%b [%d] :%b: sanity check <%s>\n",
            current->name().characters(),
            current->name().characters(),
            current->name().length(),
            current->name()[current->name().length() - 1],
            current->pid(), ch, msg ? msg : "");
    ASSERT((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'));
}
#endif

void Task::allocateLDT()
{
    ASSERT(!m_tss.ldt);
    static const WORD numLDTEntries = 4;
    WORD newLDTSelector = allocateGDTEntry();
    m_ldtEntries = new Descriptor[numLDTEntries];
#if 0
    kprintf("new ldt selector = %x\n", newLDTSelector);
    kprintf("new ldt table at = %p\n", m_ldtEntries);
    kprintf("new ldt table size = %u\n", (numLDTEntries * 8) - 1);
#endif
    Descriptor& ldt = getGDTEntry(newLDTSelector);
    ldt.setBase(m_ldtEntries);
    ldt.setLimit(numLDTEntries * 8 - 1);
    ldt.dpl = 0;
    ldt.segment_present = 1;
    ldt.granularity = 0;
    ldt.zero = 0;
    ldt.operation_size = 1;
    ldt.descriptor_type = 0;
    ldt.type = Descriptor::LDT;
    m_tss.ldt = newLDTSelector;
}

Task::Region* Task::allocateRegion(size_t size, String&& name)
{
    // FIXME: This needs sanity checks. What if this overlaps existing regions?

    auto zone = MemoryManager::the().createZone(size);
    ASSERT(zone);
    m_regions.append(make<Region>(m_nextRegion, size, move(zone), move(name)));
    m_nextRegion = m_nextRegion.offset(size).offset(16384);
    return m_regions.last().ptr();
}

Task::Task(void (*e)(), const char* n, IPC::Handle h, RingLevel ring)
    : m_name(n)
    , m_entry(e)
    , m_pid(next_pid++)
    , m_handle(h)
    , m_state(Runnable)
    , m_ring(ring)
{
    m_nextRegion = LinearAddress(0x600000);

    Region* codeRegion = nullptr;
    if (!isRing0()) {
        codeRegion = allocateRegion(4096, "code");
        ASSERT(codeRegion);
        bool success = copyToZone(*codeRegion->zone, (void*)e, PAGE_SIZE);
        ASSERT(success);
    }

    memset(&m_tss, 0, sizeof(m_tss));
    memset(&m_ldtEntries, 0, sizeof(m_ldtEntries));

    if (ring == Ring3) {
        allocateLDT();
    }

    // Only IF is set when a task boots.
    m_tss.eflags = 0x0202;

    WORD dataSegment;
    WORD stackSegment;
    WORD codeSegment;

    if (ring == Ring0) {
        codeSegment = 0x08;
        dataSegment = 0x10;
        stackSegment = dataSegment;
    } else {
        codeSegment = 0x1b;
        dataSegment = 0x23;
        stackSegment = dataSegment;
    }

    m_tss.ds = dataSegment;
    m_tss.es = dataSegment;
    m_tss.fs = dataSegment;
    m_tss.gs = dataSegment;
    m_tss.ss = stackSegment;
    m_tss.cs = codeSegment;

    ASSERT((codeSegment & 3) == (stackSegment & 3));

    m_tss.cr3 = MemoryManager::the().pageDirectoryBase().get();

    if (isRing0()) {
        m_tss.eip = (DWORD)m_entry;
    } else {
        m_tss.eip = codeRegion->linearAddress.get();
    }

    // NOTE: Each task gets 16KB of stack.
    static const DWORD defaultStackSize = 16384;

    if (isRing0()) {
        // FIXME: This memory is leaked.
        // But uh, there's also no kernel task termination, so I guess it's not technically leaked...

        dword stackBottom = (dword)kmalloc(defaultStackSize);
        m_stackTop = (stackBottom + defaultStackSize) & 0xffffff8;
        m_tss.esp = m_stackTop;
    } else {
        auto* region = allocateRegion(defaultStackSize, "stack");
        ASSERT(region);
        m_stackTop = region->linearAddress.offset(defaultStackSize).get() & 0xfffffff8;
        m_tss.esp = m_stackTop;
    }

    if (ring == Ring3) {
        // Set up a separate stack for Ring0.
        // FIXME: Don't leak this stack either.
        m_kernelStack = kmalloc(defaultStackSize);
        DWORD ring0StackTop = ((DWORD)m_kernelStack + defaultStackSize) & 0xffffff8;
        m_tss.ss0 = 0x10;
        m_tss.esp0 = ring0StackTop;
    }

    // HACK: Ring2 SS in the TSS is the current PID.
    m_tss.ss2 = m_pid;

    m_farPtr.offset = 0x12345678;

    // Don't add task 0 (kernel dummy task) to task list.
    // FIXME: This doesn't belong in the constructor.
    if (m_pid == 0)
        return;

    // Add it to head of task list (meaning it's next to run too, ATM.)
    s_tasks->prepend(this);

    system.nprocess++;

    kprintf("Task %u (%s) spawned @ %p\n", m_pid, m_name.characters(), m_tss.eip);
}

Task::~Task()
{
    system.nprocess--;
    delete [] m_ldtEntries;
    m_ldtEntries = nullptr;

    // FIXME: The task's kernel stack is currently leaked, because otherwise we GPF.
    //        This obviously needs figuring out.
#if 0
    if (m_kernelStack) {
        kfree(m_kernelStack);
        m_kernelStack = nullptr;
    }
#endif
}

void Task::dumpRegions()
{
    kprintf("Task %s(%u) regions:\n", name().characters(), pid());
    kprintf("BEGIN       END         SIZE        NAME\n");
    for (auto& region : m_regions) {
        kprintf("%x -- %x    %x    %s\n",
            region->linearAddress.get(),
            region->linearAddress.offset(region->size - 1).get(),
            region->size,
            region->name.characters());
    }
}

void Task::sys$exit(int status)
{
    cli();
    kprintf("sys$exit: %s(%u) exit with status %d\n", name().characters(), pid(), status);

    setState(Exiting);
    dumpRegions();

    s_tasks->remove(this);

    if (!scheduleNewTask()) {
        kprintf("Task::taskDidCrash: Failed to schedule a new task :(\n");
        HANG;
    }

    delete this;

    switchNow();
}

void Task::taskDidCrash(Task* crashedTask)
{
    // NOTE: This is called from an excepton handler, so interrupts are disabled.
    crashedTask->setState(Crashing);
    crashedTask->dumpRegions();

    s_tasks->remove(crashedTask);

    if (!scheduleNewTask()) {
        kprintf("Task::taskDidCrash: Failed to schedule a new task :(\n");
        HANG;
    }

    delete crashedTask;

    switchNow();
}

void yield()
{
    if (!current) {
        kprintf( "PANIC: yield() with !current" );
        HANG;
    }

    //kprintf("%s<%u> yield()\n", current->name().characters(), current->pid());

    cli();
    if (!scheduleNewTask()) {
        sti();
        return;
    }

    //kprintf("yield() jumping to new task: %x (%s)\n", current->farPtr().selector, current->name().characters());
    switchNow();
}

void switchNow()
{
    Descriptor& descriptor = getGDTEntry(current->selector());
    descriptor.type = 9;
    flushGDT();
    asm("sti\n"
        "ljmp *(%%eax)\n"
        ::"a"(&current->farPtr())
    );
}

bool scheduleNewTask()
{
    if (!current) {
        // XXX: The first ever context_switch() goes to the idle task.
        //      This to setup a reliable place we can return to.
        return contextSwitch(Task::kernelTask());
    }

#if 0
    kprintf("Scheduler choices:\n");
    for (auto* task = s_tasks->head(); task; task = task->next()) {
        kprintf("%p  %u  %s\n", task, task->pid(), task->name().characters());
    }
#endif

    // Check and unblock tasks whose wait conditions have been met.
    for (auto* task = s_tasks->head(); task; task = task->next()) {
        if (task->state() == Task::BlockedReceive && (task->ipc.msg.isValid() || task->ipc.notifies)) {
            task->unblock();
            continue;
        }

        if (task->state() == Task::BlockedSend) {
            Task* peer = Task::fromIPCHandle(task->ipc.dst);
            if (peer && peer->state() == Task::BlockedReceive && peer->acceptsMessageFrom(*task)) {
                task->unblock();
                continue;
            }
        }

        if (task->state() == Task::BlockedSleep) {
            if (task->wakeupTime() <= system.uptime) {
                task->unblock();
                continue;
            }
        }
    }

    auto* prevHead = s_tasks->head();
    for (;;) {
        // Move head to tail.
        s_tasks->append(s_tasks->removeHead());
        auto* task = s_tasks->head();

        if (task->state() == Task::Runnable || task->state() == Task::Running) {
            //kprintf("switch to %s (%p vs %p)\n", task->name().characters(), task, current);
            return contextSwitch(task);
        }

        if (task == prevHead) {
            // Back at task_head, nothing wants to run.
            kprintf("Switch to kernel task\n");
            return contextSwitch(Task::kernelTask());
        }
    }
}

static void drawSchedulerBanner(Task& task)
{
    return;
    // FIXME: We need a kernel lock to do stuff like this :(
    //return;
    auto c = vga_get_cursor();
    auto a = vga_get_attr();
    vga_set_cursor(0, 50);
    vga_set_attr(0x20);
    kprintf("          ");
    kprintf("          ");
    kprintf("          ");
    vga_set_cursor(0, 50);
    kprintf("pid: %u ", task.pid());
    vga_set_cursor(0, 58);
    kprintf("%s", task.name().characters());
    vga_set_cursor(0, 65);
    kprintf("eip: %p", task.tss().eip);
    vga_set_attr(a);
    vga_set_cursor(c);
}

static bool contextSwitch(Task* t)
{
    //kprintf("c_s to %s (same:%u)\n", t->name().characters(), current == t);
    t->setTicksLeft(5);

    if (current == t)
        return false;

    // Some sanity checking to force a crash earlier.
    auto csRPL = t->tss().cs & 3;
    auto ssRPL = t->tss().ss & 3;
    ASSERT(csRPL == ssRPL);

    if (current) {
        // If the last task hasn't blocked (still marked as running),
        // mark it as runnable for the next round.
        if (current->state() == Task::Running)
            current->setState(Task::Runnable);

        bool success = MemoryManager::the().unmapRegionsForTask(*current);
        ASSERT(success);
    }

    bool success = MemoryManager::the().mapRegionsForTask(*t);
    ASSERT(success);

    current = t;
    t->setState(Task::Running);

    if (!t->selector())
        t->setSelector(allocateGDTEntry());

    auto& tssDescriptor = getGDTEntry(t->selector());

    tssDescriptor.limit_hi = 0;
    tssDescriptor.limit_lo = 0xFFFF;
    tssDescriptor.base_lo = (DWORD)(&t->tss()) & 0xFFFF;
    tssDescriptor.base_hi = ((DWORD)(&t->tss()) >> 16) & 0xFF;
    tssDescriptor.base_hi2 = ((DWORD)(&t->tss()) >> 24) & 0xFF;
    tssDescriptor.dpl = 0;
    tssDescriptor.segment_present = 1;
    tssDescriptor.granularity = 1;
    tssDescriptor.zero = 0;
    tssDescriptor.operation_size = 1;
    tssDescriptor.descriptor_type = 0;
    tssDescriptor.type = 11; // Busy TSS

    flushGDT();
    drawSchedulerBanner(*t);

    t->didSchedule();
    return true;
}

Task* Task::fromPID(pid_t pid)
{
    for (auto* task = s_tasks->head(); task; task = task->next()) {
        if (task->pid() == pid)
            return task;
    }
    return nullptr;
}

Task* Task::fromIPCHandle(IPC::Handle handle)
{
    for (auto* task = s_tasks->head(); task; task = task->next()) {
        if (task->handle() == handle)
            return task;
    }
    return nullptr;
}

FileHandle* Task::fileHandleIfExists(int fd)
{
    if (fd < 0)
        return nullptr;
    if ((unsigned)fd < m_fileHandles.size())
        return m_fileHandles[fd].ptr();
    return nullptr;
}

int Task::sys$seek(int fd, int offset)
{
    auto* handle = fileHandleIfExists(fd);
    if (!handle)
        return -1;
    return handle->seek(offset, SEEK_SET);
}

ssize_t Task::sys$read(int fd, void* outbuf, size_t nread)
{
    Task::checkSanity("Task::sys$read");
    kprintf("Task::sys$read: called(%d, %p, %u)\n", fd, outbuf, nread);
    auto* handle = fileHandleIfExists(fd);
    kprintf("Task::sys$read: handle=%p\n", handle);
    if (!handle) {
        kprintf("Task::sys$read: handle not found :(\n");
        return -1;
    }
    kprintf("call read on handle=%p\n", handle);
    nread = handle->read((byte*)outbuf, nread);
    kprintf("called read\n");
    kprintf("Task::sys$read: nread=%u\n", nread);
    return nread;
}

int Task::sys$close(int fd)
{
    auto* handle = fileHandleIfExists(fd);
    if (!handle)
        return -1;
    // FIXME: Implement.
    return 0;
}

int Task::sys$open(const char* path, size_t pathLength)
{
    Task::checkSanity("sys$open");
    kprintf("Task::sys$open(): PID=%u, path=%s {%u}\n", m_pid, path, pathLength);
    auto* handle = current->openFile(String(path, pathLength));
    if (handle)
        return handle->fd();
    return -1;
}

FileHandle* Task::openFile(String&& path)
{
    kprintf("calling vfs::open with vfs=%p, path='%s'\n", &VirtualFileSystem::the(), path.characters());
    auto handle = VirtualFileSystem::the().open(move(path));
    if (!handle) {
        kprintf("vfs::open() failed\n");
        return nullptr;
    }
    handle->setFD(m_fileHandles.size());
    kprintf("vfs::open() worked! handle=%p, fd=%d\n", handle.ptr(), handle->fd());
    m_fileHandles.append(move(handle)); // FIXME: allow non-move Vector::append
    return m_fileHandles.last().ptr();
}

int Task::sys$kill(pid_t pid, int sig)
{
    (void) sig;
    if (pid == 0) {
        // FIXME: Send to same-group processes.
        ASSERT(pid != 0);
    }
    if (pid == -1) {
        // FIXME: Send to all processes.
        ASSERT(pid != -1);
    }
    ASSERT_NOT_REACHED();
    Task* peer = Task::fromPID(pid);
    if (!peer) {
        // errno = ESRCH;
        return -1;
    }
#if 0
    send(peer->handle(), IPC::Message(SYS_KILL, DataBuffer::copy((BYTE*)&sig, sizeof(sig))));
    IPC::Message response = receive(peer->handle());
    return *(int*)response.data();
#endif
    return -1;
}

uid_t Task::sys$getuid()
{
    return m_uid;
}

bool Task::acceptsMessageFrom(Task& peer)
{
    return !ipc.msg.isValid() && (ipc.src == IPC::Handle::Any || ipc.src == peer.handle());
}

void Task::unblock()
{
    ASSERT(m_state != Task::Runnable && m_state != Task::Running);
    system.nblocked--;
    m_state = Task::Runnable;
}

void Task::block(Task::State state)
{
    ASSERT(current->state() == Task::Running);
    system.nblocked++;
    current->setState(state);
}

void block(Task::State state)
{
    current->block(state);
    yield();
}

void sleep(DWORD ticks)
{
    ASSERT(current->state() == Task::Running);
    current->setWakeupTime(system.uptime + ticks);
    current->block(Task::BlockedSleep);
    yield();
}

void Task::sys$sleep(DWORD ticks)
{
    ASSERT(this == current);
    sleep(ticks);
}

Task* Task::kernelTask()
{
    ASSERT(s_kernelTask);
    return s_kernelTask;
}

void Task::setError(int error)
{
    m_error = error;
}

Task::Region::Region(LinearAddress a, size_t s, RetainPtr<Zone>&& z, String&& n)
    : linearAddress(a)
    , size(s)
    , zone(move(z))
    , name(move(n))
{
}

Task::Region::~Region()
{
}