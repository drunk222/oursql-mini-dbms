#include "oursql/common/status.h"

#include <utility>

namespace oursql {

Status::Status(ErrorCode code, std::string message) noexcept : code_(code), message_(std::move(message)) {}

Status Status::Ok() noexcept {
  return Status{};
}

Status Status::InvalidArgument(std::string message) {
  return Status(ErrorCode::InvalidArgument, std::move(message));
}

Status Status::NotFound(std::string message) {
  return Status(ErrorCode::NotFound, std::move(message));
}

Status Status::NotImplemented(std::string message) {
  return Status(ErrorCode::NotImplemented, std::move(message));
}

Status Status::AlreadyExists(std::string message) {
  return Status(ErrorCode::AlreadyExists, std::move(message));
}

Status Status::TypeMismatch(std::string message) {
  return Status(ErrorCode::TypeMismatch, std::move(message));
}

Status Status::OutOfSpace(std::string message) {
  return Status(ErrorCode::OutOfSpace, std::move(message));
}

Status Status::RecordTooLarge(std::string message) {
  return Status(ErrorCode::RecordTooLarge, std::move(message));
}

Status Status::IOError(std::string message) {
  return Status(ErrorCode::IOError, std::move(message));
}

Status Status::InternalError(std::string message) {
  return Status(ErrorCode::InternalError, std::move(message));
}

bool Status::ok() const noexcept {
  return code_ == ErrorCode::Ok;
}

ErrorCode Status::code() const noexcept {
  return code_;
}

const std::string &Status::message() const noexcept {
  return message_;
}

std::string Status::ToString() const {
  switch (code_) {
    case ErrorCode::Ok:
      return "Ok";
    case ErrorCode::InvalidArgument:
      return message_.empty() ? "InvalidArgument" : "InvalidArgument: " + message_;
    case ErrorCode::NotFound:
      return message_.empty() ? "NotFound" : "NotFound: " + message_;
    case ErrorCode::NotImplemented:
      return message_.empty() ? "NotImplemented" : "NotImplemented: " + message_;
    case ErrorCode::AlreadyExists:
      return message_.empty() ? "AlreadyExists" : "AlreadyExists: " + message_;
    case ErrorCode::TypeMismatch:
      return message_.empty() ? "TypeMismatch" : "TypeMismatch: " + message_;
    case ErrorCode::OutOfSpace:
      return message_.empty() ? "OutOfSpace" : "OutOfSpace: " + message_;
    case ErrorCode::RecordTooLarge:
      return message_.empty() ? "RecordTooLarge" : "RecordTooLarge: " + message_;
    case ErrorCode::IOError:
      return message_.empty() ? "IOError" : "IOError: " + message_;
    case ErrorCode::InternalError:
      return message_.empty() ? "InternalError" : "InternalError: " + message_;
  }
  return "InternalError: unknown code";
}

Status::operator bool() const noexcept {
  return ok();
}

}  // namespace oursql
