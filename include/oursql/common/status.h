#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace oursql {

enum class ErrorCode {
  Ok,
  InvalidArgument,
  NotFound,
  NotImplemented,
  AlreadyExists,
  TypeMismatch,
  OutOfSpace,
  IOError,
  InternalError,
};

class Status {
 public:
  Status() noexcept = default;

  // 调用者：上层控制器；作用：构造成功状态；返回：表示成功的状态对象。
  static Status Ok() noexcept;

  // 调用者：上层控制器；作用：构造参数错误；返回：带上下文的错误状态。
  static Status InvalidArgument(std::string message);

  // 调用者：上层控制器；作用：构造未找到错误；返回：带上下文的错误状态。
  static Status NotFound(std::string message);

  // 调用者：上层控制器；作用：构造未实现错误；返回：带上下文的错误状态。
  static Status NotImplemented(std::string message);

  // 调用者：上层控制器；作用：构造已存在错误；返回：带上下文的错误状态。
  static Status AlreadyExists(std::string message);

  // 调用者：上层控制器；作用：构造类型不匹配错误；返回：带上下文的错误状态。
  static Status TypeMismatch(std::string message);

  // 调用者：页面或存储层；作用：表示可用空间不足；返回：带上下文的错误状态。
  static Status OutOfSpace(std::string message);

  // 调用者：上层控制器；作用：构造 I/O 错误；返回：带上下文的错误状态。
  static Status IOError(std::string message);

  // 调用者：内部代码；作用：构造内部错误；返回：带上下文的错误状态。
  static Status InternalError(std::string message);

  [[nodiscard]] bool ok() const noexcept;
  [[nodiscard]] ErrorCode code() const noexcept;
  [[nodiscard]] const std::string &message() const noexcept;
  [[nodiscard]] std::string ToString() const;
  explicit operator bool() const noexcept;

 private:
  Status(ErrorCode code, std::string message) noexcept;

  ErrorCode code_{ErrorCode::Ok};
  std::string message_;
};

template <typename T>
class Result {
 public:
  // 调用者：上层模块；作用：构造成功结果；返回：携带值的结果对象。
  explicit Result(T value) : status_(Status::Ok()), value_(std::move(value)) {}

  // 调用者：上层模块；作用：构造错误结果；返回：携带错误的结果对象。
  explicit Result(Status status) : status_(std::move(status)) {
    if (status_.ok()) {
      status_ = Status::InternalError("Result 不能包裹空值的成功状态");
    }
  }

  [[nodiscard]] bool ok() const noexcept {
    return status_.ok();
  }

  [[nodiscard]] const Status &status() const noexcept {
    return status_;
  }

  [[nodiscard]] T &value() & {
    EnsureValue();
    return *value_;
  }

  [[nodiscard]] const T &value() const & {
    EnsureValue();
    return *value_;
  }

  [[nodiscard]] T &&value() && {
    EnsureValue();
    return std::move(*value_);
  }

 private:
  void EnsureValue() const {
    if (!ok() || !value_.has_value()) {
      throw std::logic_error("Result 在未成功时访问 value()");
    }
  }

  Status status_;
  std::optional<T> value_;
};

}  // namespace oursql
