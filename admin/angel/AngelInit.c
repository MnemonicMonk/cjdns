/* vim: set expandtab ts=4 sw=4: */
/*
 * You may redistribute this program and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "admin/angel/Angel.h"
#include "admin/angel/InterfaceWaiter.h"
#include "benc/Dict.h"
#include "benc/String.h"
#include "benc/serialization/standard/BencMessageReader.h"
#include "benc/serialization/standard/BencMessageWriter.h"
#include "crypto/random/Random.h"
#include "interface/FramingIface.h"
#include "io/FileWriter.h"
#include "memory/Allocator.h"
#include "memory/MallocAllocator.h"
#include "exception/Except.h"
#include "util/events/EventBase.h"
#include "util/events/Pipe.h"
#include "util/events/Process.h"
#include "util/Bits.h"
#include "util/Assert.h"
#include "util/Hex.h"
#include "util/log/WriterLog.h"
#include "util/Security.h"
#include "wire/Message.h"

#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>

static void initCore(char* coreBinaryPath,
                     String* corePipeName,
                     struct EventBase* base,
                     struct Allocator* alloc,
                     struct Except* eh)
{
    char* args[] = { "core", corePipeName->bytes, NULL };

    FILE* file;
    if ((file = fopen(coreBinaryPath, "r")) != NULL) {
        fclose(file);
    } else {
        Except_throw(eh, "Can't open core executable [%s] for reading.", coreBinaryPath);
    }

    if (Process_spawn(coreBinaryPath, args, base, alloc)) {
        Except_throw(eh, "Failed to spawn core process.");
    }
}

static void setUser(char* user, struct Log* logger, struct Except* eh)
{
    int res = 0;
    switch ((res = Security_setUser(user, logger, eh))) {
        case Security_setUser_PERMISSION: return;
        case 0: return;
        default:;
    }
    Except_throw(eh, "Security_setUser() returned unknown result [%d]", res);
}

static struct Pipe* getClientPipe(int argc,
                                  char** argv,
                                  struct EventBase* base,
                                  struct Except* eh,
                                  struct Allocator* alloc)
{
    int inFromClientNo;
    int outToClientNo;
    if (argc < 4 || (inFromClientNo = atoi(argv[2])) == 0) {
        inFromClientNo = STDIN_FILENO;
    }
    if (argc < 4 || (outToClientNo = atoi(argv[3])) == 0) {
        outToClientNo = STDOUT_FILENO;
    }

    // named pipe.
    if (argc > 2 && inFromClientNo == STDIN_FILENO) {
        return Pipe_named(argv[2], base, eh, alloc);
    }
    return Pipe_forFiles(inFromClientNo, outToClientNo, base, eh, alloc);
}

static void coreDied(struct Pipe* p, int status)
{
    exit(1);
}

static void clientDisconnected(struct Pipe* p, int status)
{
    fprintf(stdout, "Cjdns has started up in the background\n");
}

/**
 * Input:
 * {
 *   "admin": {
 *     "core": "/path/to/core/binary",
 *     "bind": "127.0.0.1:12345",
 *     "pass": "12345adminsocketpassword",
 *     "user": "setUidToThisUser"
 *   }
 * }
 * for example:
 * d5:admind4:core30:./build/admin/angel/cjdns-core4:bind15:127.0.0.1:123454:pass4:abcdee
 *
 * Pre-existing core mode:
 * {
 *   "admin": {
 *     "core": {
 *       "fromCore": 12,
 *       "toCore": 14
 *     },
 *     "bind": "127.0.0.1:12345",
 *     "pass": "12345adminsocketpassword",
 *     "user": "setUidToThisUser"
 *   }
 * }
 *
 * If "core" is a dictionary, the angel will behave as though the core is already spawned and
 * it will read from the core on the file descriptor given by "fromCore" and write to the file
 * given by "toCore".
 *
 * "user" is optional, if set the angel will setuid() that user's uid.
 */
int AngelInit_main(int argc, char** argv)
{
    struct Except* eh = NULL;

    struct Allocator* alloc = MallocAllocator_new(1<<21);
    struct Writer* logWriter = FileWriter_new(stdout, alloc);
    struct Log* logger = WriterLog_new(logWriter, alloc);
    struct Random* rand = Random_new(alloc, logger, eh);
    Allocator_setCanary(alloc, (long)Random_int64(rand));
    struct Allocator* tempAlloc = Allocator_child(alloc);
    struct EventBase* eventBase = EventBase_new(alloc);

    struct Pipe* clientPipe = getClientPipe(argc, argv, eventBase, eh, alloc);
    clientPipe->logger = logger;
    clientPipe->onClose = clientDisconnected;

    Log_debug(logger, "Getting pre-configuration from client");

    struct Message* preConf = InterfaceWaiter_waitForData(&clientPipe->iface, eventBase, alloc, eh);

    Log_debug(logger, "Finished getting pre-configuration from client");

    Dict* config = BencMessageReader_read(preConf, tempAlloc, eh);

    Dict* admin = Dict_getDict(config, String_CONST("admin"));
    String* core = Dict_getString(admin, String_CONST("core"));
    String* bind = Dict_getString(admin, String_CONST("bind"));
    String* pass = Dict_getString(admin, String_CONST("pass"));
    String* user = Dict_getString(admin, String_CONST("user"));
    String* corePipeName = Dict_getString(admin, String_CONST("corePipeName"));

    if (!bind || !pass || (!core && !corePipeName)) {
        Except_throw(eh, "missing configuration params in preconfig. [%s]", preConf->bytes);
    }

    if (!corePipeName) {
        char name[32] = {0};
        Random_base32(rand, (uint8_t*)name, 31);
        corePipeName = String_new(name, tempAlloc);
    }

    struct Pipe* corePipe = Pipe_named(corePipeName->bytes, eventBase, eh, alloc);
    corePipe->logger = logger;
    corePipe->onClose = coreDied;
    struct Iface* coreIface = FramingIface_new(65535, &corePipe->iface, alloc);

    if (core) {
        Log_info(logger, "Initializing core [%s]", core->bytes);
        initCore(core->bytes, corePipeName, eventBase, alloc, eh);
    }

    Log_debug(logger, "Sending pre-configuration to core.");

    struct Message* msg = Message_new(0, 1024, tempAlloc);
    BencMessageWriter_write(config, msg, eh);
    Log_keys(logger, "Sent [%d] bytes to core", msg->length);
    Iface_CALL(coreIface->send, msg, coreIface);

    struct Message* coreResponse = InterfaceWaiter_waitForData(coreIface, eventBase, tempAlloc, eh);

    Iface_CALL(clientPipe->iface.send, coreResponse, &clientPipe->iface);

    #ifdef Log_KEYS
        uint8_t lastChar = coreResponse->bytes[coreResponse->length-1];
        coreResponse->bytes[coreResponse->length-1] = 0;
        Log_keys(logger, "Sent [%s%c] to client.", coreResponse->bytes, lastChar);
        coreResponse->bytes[coreResponse->length-1] = lastChar;
    #endif

    if (user) {
        setUser(user->bytes, logger, eh);
    }

    Allocator_free(tempAlloc);
    Log_debug(logger, "Angel_start()");
    Angel_start(coreIface, eventBase, logger, alloc);
    return 0;
}
