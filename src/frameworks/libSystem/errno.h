#pragma once

#include <cerrno>

namespace orchard
{
constexpr int EPERM_ = 1, ENOENT_ = 2, ESRCH_ = 3, EINTR_ = 4, EIO_ = 5, EBADF_ = 9, ECHILD_ = 10, EDEADLK_ = 11, ENOMEM_ = 12,
              EACCES_ = 13, EFAULT_ = 14, EBUSY_ = 16, EEXIST_ = 17, EXDEV_ = 18, ENOTDIR_ = 20, EISDIR_ = 21, EINVAL_ = 22, ENFILE_ = 23,
              EMFILE_ = 24, ENOTTY_ = 25, EFBIG_ = 27, ENOSPC_ = 28, ESPIPE_ = 29, EROFS_ = 30, EPIPE_ = 32, EDOM_ = 33, ERANGE_ = 34,
              EAGAIN_ = 35, EINPROGRESS_ = 36, ENOTSOCK_ = 38, EAFNOSUPPORT_ = 47, ECONNREFUSED_ = 61, ENETUNREACH_ = 51, ENOTSUP_ = 45,
              ETIMEDOUT_ = 60, ELOOP_ = 62, ENAMETOOLONG_ = 63, ENOTEMPTY_ = 66, ENOSYS_ = 78;

inline int darwin_errno(int host)
{
    switch (host)
    {
    case EPERM: return EPERM_;
    case ENOENT: return ENOENT_;
    case EIO: return EIO_;
    case EBADF: return EBADF_;
    case ENOMEM: return ENOMEM_;
    case EACCES: return EACCES_;
    case EBUSY: return EBUSY_;
    case EEXIST: return EEXIST_;
    case EXDEV: return EXDEV_;
    case ENOTDIR: return ENOTDIR_;
    case EISDIR: return EISDIR_;
    case EINVAL: return EINVAL_;
    case EMFILE: return EMFILE_;
    case ENOSPC: return ENOSPC_;
    case ESPIPE: return ESPIPE_;
    case EROFS: return EROFS_;
    case EPIPE: return EPIPE_;
    case ERANGE: return ERANGE_;
    case EAGAIN: return EAGAIN_;
    case ENAMETOOLONG: return ENAMETOOLONG_;
    case ENOTEMPTY: return ENOTEMPTY_;
    default: return EIO_;
    }
}

}
