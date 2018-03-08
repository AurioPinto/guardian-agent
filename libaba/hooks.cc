#include "guardo.pb.h"
#include "guardian_agent.pb.h"
#include "socket.hh"
#include "util.hh"

#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <experimental/filesystem>
#include <fcntl.h>
#include <libsyscall_intercept_hook_point.h>
#include <syscall.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace guardian_agent {

namespace fs = std::experimental::filesystem;
namespace proto = google::protobuf;

static const char* AGENT_GUARD_SOCK_NAME = ".agent-guard-sock";
static const char* GUARDO_SOCK_NAME = ".guardo-sock";


std::unique_ptr<FileDescriptor> marshal_fds(Operation* op, std::vector<int>* fds) {
    std::unique_ptr<FileDescriptor> fd_cwd;
    for (auto& arg : *op->mutable_args()) {
        if (arg.has_dir_fd_arg()) {
            DirFd* dir_fd = arg.mutable_dir_fd_arg();
            if (dir_fd->form_case() != DirFd::kFd) {
                continue;
            }
            if (dir_fd->fd() == AT_FDCWD) {
                if (!fd_cwd) {
                    fd_cwd = std::make_unique<FileDescriptor>(openat(AT_FDCWD, ".", O_RDONLY, 0));
                    if (fd_cwd->fd_num() < 0) {
                        throw unix_error("Failed to duplicate AT_FDCWD");
                    }
                }
                fds->push_back(fd_cwd->fd_num());
                dir_fd->set_path(fs::current_path());
            } else {
                fds->push_back(dir_fd->fd());
                dir_fd->set_path(fs::read_symlink(fs::path("/proc/self/fd") / std::to_string(dir_fd->fd())));
            }
        } else if (arg.has_socket_arg()) {
            Socket* sock = arg.mutable_socket_arg();
            fds->push_back(sock->fd());
            sock->clear_fd();
        }   
    }

    return fd_cwd;
}

void create_open_op(int dir_fd, 
                    const char* path, 
                    int flags,
                    int mode,
                    Operation* op)
{
    op->add_args()->mutable_dir_fd_arg()->set_fd(dir_fd);
    op->add_args()->set_string_arg(path);
    op->add_args()->set_int_arg(flags);
    op->add_args()->set_int_arg(mode);
}

void create_unlink_op(int dir_fd, 
                      const char* path, 
                      int flags,
                      Operation* op) 
{
    op->add_args()->mutable_dir_fd_arg()->set_fd(dir_fd);
    op->add_args()->set_string_arg(path);
    op->add_args()->set_int_arg(flags);
}

bool create_access_op(int dir_fd, 
                      const char* path, 
                      int mode,
                      int flags,
                      Operation* op) 
{
    // Don't try to elevate executable access checks for files that
    // are not executable at all. 
    if (mode == X_OK) {
        auto p = fs::status(path).permissions();
        if (((p & fs::perms::owner_exec) == fs::perms::none) &&
            ((p & fs::perms::group_exec) == fs::perms::none) &&
            ((p & fs::perms::others_exec) == fs::perms::none)) {
            return false;
        }
    }
    op->add_args()->mutable_dir_fd_arg()->set_fd(dir_fd);
    op->add_args()->set_string_arg(path);
    op->add_args()->set_int_arg(mode);
    op->add_args()->set_int_arg(flags);
    return true;
}

void create_socket_op(int domain, int type, int protocol, Operation* op)
{
    op->add_args()->set_int_arg(domain);
    op->add_args()->set_int_arg(type);
    op->add_args()->set_int_arg(protocol);
}

void create_bind_op(int sockfd, const struct sockaddr *addr, socklen_t addrlen, 
                    Operation* op)
{
    op->add_args()->mutable_socket_arg()->set_fd(sockfd);
    op->add_args()->set_bytes_arg(std::string((const char*)addr, addrlen));
}


fs::path user_runtime_dir()
{
    const char* dir = std::getenv("XDG_RUNTIME_DIR");
    if (dir == NULL) {
        dir = std::getenv("HOME");
    }
    return fs::path(std::string(dir));
}

std::string create_raw_msg(unsigned char msg_num, const google::protobuf::MessageLite& msg)
{
    std::string raw_msg(5, '\0');
    *(unsigned char*)(raw_msg.data() + sizeof(int)) = msg_num;
    msg.AppendToString(&raw_msg);
    *(int*)raw_msg.data() = htonl(raw_msg.size() - sizeof(int));
    return raw_msg;    
}

bool read_expected_msg(FileDescriptor* fd, const unsigned char expected_msg_num, google::protobuf::MessageLite* msg) 
{
    std::string packet_len_buf = fd->read_full(sizeof(int));
    int packet_len = ntohl(*(int*)packet_len_buf.data());
    std::string packet = fd->read_full(packet_len);
    if (packet[0] != expected_msg_num) {
        std::cerr << "Invalid msg_num, expected: " << expected_msg_num << ", got: " << packet[0] << std::endl;
        return false;
    }
    if (!msg->ParseFromString(packet.substr(1))) {
        std::cerr << "Failed to parse msg " << expected_msg_num << " from string" << std::endl;
        return false;
    }
    return true;
}

bool read_expected_msg_with_fd(UnixSocket* socket, const unsigned char expected_msg_num, google::protobuf::MessageLite* msg, std::vector<int>* fds) 
{
    std::string response_data = socket->recvmsg(fds);
    int payload_size = ntohl(*(int*)response_data.data());
    if (response_data.size() != (sizeof(int) + payload_size)) {
        std::cerr << "Error: enexpected data size: " << response_data.size()  
            << " payload size: " << payload_size << std::endl;
        return false;
    }
    unsigned char msg_num = *(response_data.data() + sizeof(payload_size));
    if (msg_num != expected_msg_num) {
        std::cerr << "Invalid msg_num, expected: " << expected_msg_num << ", got: " << msg_num << std::endl;
        return false;
    }
    if (!msg->ParseFromString(response_data.data() + sizeof(payload_size) + sizeof(msg_num))) {
        std::cerr << "Error: failed to parse msg:" << msg_num << std::endl;
        return false;
    }
    return true;
}

bool get_credential(const Operation& op, const Challenge& challenge, Credential* credential) 
{
    UnixSocket socket;
    socket.connect(Address::NewUnixAddress(user_runtime_dir() / AGENT_GUARD_SOCK_NAME));

    CredentialRequest request;
    *request.mutable_op() = op;
    *request.mutable_challenge() = challenge;
    socket.write(create_raw_msg(CREDENTIAL_REQUEST, request), true);

    CredentialResponse response;
    if (!read_expected_msg(&socket, CREDENTIAL_RESPONSE, &response)) {
        std::cerr << "Failed to read CredentialResponse" << std::endl;
        return false;
    }
    if (response.status() != CredentialResponse_Status_APPROVED) {
        std::cerr << "Credential request not approved: " << CredentialResponse_Status_Name(response.status()) << std::endl;
        return false;
    }

    *credential = response.credential();
    return true;
}

static void hook(long syscall_number,
                 long arg0, 
                 long arg1,
                 long arg2, 
                 long arg3, 
                 __attribute__((unused)) long arg4, 
                 __attribute__((unused)) long arg5,
                long int* result)
{
    Operation op;
    std::vector<int> fds;
    bool should_hook = true;
    op.set_syscall_num(syscall_number);
    switch (syscall_number) {
	// Must be in sync with switch statement in 'safe_hook' below.
        case SYS_open: 
            create_open_op(AT_FDCWD, (char*)arg0, arg1, arg2, &op);
            break;
        case SYS_openat:
            create_open_op((int)arg0, (char*)arg1, arg2, arg3, &op);
            break;
        case SYS_unlink:
            create_unlink_op(AT_FDCWD, (char*)arg0, 0, &op);
            break;
        case SYS_unlinkat:
            create_unlink_op((int)arg0, (char*)arg1, arg2, &op);
            break;
        case SYS_access:
            should_hook = create_access_op(AT_FDCWD, (char*)arg0, (int)arg1, 0, &op);
            break;
        case SYS_faccessat:
            should_hook = create_access_op((int)arg0, (char*)arg1, (int)arg2, (int)arg3, &op);
            break;
        case SYS_socket:
            create_socket_op((int)arg0, (int)arg1, (int)arg2, &op);
            break;
        case SYS_bind:
            create_bind_op((int)arg0, (sockaddr*)arg1, (socklen_t)arg2, &op);
            break;
        default:
            std::cerr << "Error: unexpected intercepted syscall: " << syscall_number << std::endl;
            return;
    }

    if (!should_hook) {
        return;
    }

    auto fd_cwd = marshal_fds(&op, &fds);

    UnixSocket socket;
    socket.connect(Address::NewUnixAddress(fs::path("/tmp") / GUARDO_SOCK_NAME));    

    ChallengeRequest challenge_req;
    socket.sendmsg(create_raw_msg(CHALLENGE_REQUEST, challenge_req), std::vector<int>());
    Challenge challenge;
    if (!read_expected_msg(&socket, CHALLENGE_RESPONSE, &challenge)) {
        std::cerr << "Failed to get challenge" << std::endl;
        return;
    }

    Credential credential;
    if (!get_credential(op, challenge, &credential)) {
        return;
    }

    ElevationRequest elevation_request;
    *elevation_request.mutable_op() = op;
    *elevation_request.mutable_credential() = credential;
    socket.sendmsg(create_raw_msg(ELEVATION_REQUEST, elevation_request), fds);

    fds.clear();
    
    ElevationResponse elevation_response;
    if (!read_expected_msg_with_fd(&socket, ELEVATION_RESPONSE, &elevation_response, &fds)) {
        return;
    }

    if (elevation_response.is_result_fd()) {
        if (fds.size() == 0) 
        {
            std::cerr << "Error: no file descriptor with approval" << std::endl;
            return;
        }
        *result = fds[0];
    } else {
        *result = elevation_response.result();
    }
    return;
}


static int safe_hook(long syscall_number,
                     long arg0, 
                     long arg1,
                     long arg2, 
                     long arg3, 
                     long arg4, 
                     long arg5,
                     long *result)
{
    switch (syscall_number) {
    case SYS_open: 
    case SYS_openat:
    case SYS_unlink:
    case SYS_unlinkat:
    case SYS_access:
    case SYS_socket:
    case SYS_bind:
        break;
    default:
        return 1;
    }

    long real_result = syscall_no_intercept(syscall_number, arg0, arg1, arg2, arg3, arg4, arg5);
    *result = real_result;
    if ((real_result != -EACCES) && (real_result != -EPERM)) {
        return 0;
    }

    try {
        hook(syscall_number, arg0, arg1, arg2, arg3, arg4, arg5, result);
    } catch ( const std::exception & e ) { /* don't throw from hook */
        print_exception( e );
    } catch (...) {
        std::cerr << "Unknown exeception caught in hook" << std::endl;
    }
    return 0;
}

} // namespace guardian_agent

static __attribute__((constructor)) void
init(void)
{
	// Set up the callback function
	intercept_hook_point = guardian_agent::safe_hook;
}
