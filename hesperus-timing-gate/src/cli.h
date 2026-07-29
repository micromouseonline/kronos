#pragma once

#include <Arduino.h>
#include <string.h>

struct CliCommand {
  const char *name;
  void (*handler)(int argc, char **argv);
};

struct CliAlias {
  const char *name;
  void (*handler)(int argc, char **argv);
};

class Cli {
 public:
  static constexpr size_t MAX_LINE = 64;
  static constexpr size_t MAX_ARGS = 8;

  void begin(const CliCommand *commands, size_t count,
             const CliAlias *aliases = nullptr, size_t aliasCount = 0,
             Stream &serial = Serial) {
    _commands = commands;
    _commandCount = count;
    _aliases = aliases;
    _aliasCount = aliasCount;
    _serial = &serial;
    _len = 0;
  }

  void poll() {
    while (_serial->available()) {
      handleChar(_serial->read());
    }
  }

 private:
  void handleChar(char c) {
    if (c == '\r') {
      return;
    } else if (c == '\n') {
      _serial->write('\r');
      _serial->write('\n');
      _buf[_len] = '\0';
      if (_len > 0) {
        dispatch();
      }
      _len = 0;
      return;
    } else if (c == '\b' || c == 0x7f) {
      if (_len > 0) {
        _len--;
        _serial->write('\b');
        _serial->write(' ');
        _serial->write('\b');
      }
      return;
    } else if (c >= 32 && c < 127) {
      if (_len < MAX_LINE - 1) {
        _buf[_len++] = c;
        _serial->write(c);
      }
      return;
    }
  }

  void dispatch() {
    char *argv[MAX_ARGS];
    int argc = tokenize(_buf, argv, MAX_ARGS);

    if (argc == 0) {
      return;
    }

    for (size_t i = 0; i < _commandCount; i++) {
      if (strcmp(argv[0], _commands[i].name) == 0) {
        _commands[i].handler(argc, argv);
        return;
      }
    }

    for (size_t i = 0; i < _aliasCount; i++) {
      if (strcmp(argv[0], _aliases[i].name) == 0) {
        _aliases[i].handler(argc, argv);
        return;
      }
    }

    _serial->print("unknown command: ");
    _serial->println(argv[0]);
  }

  static int tokenize(char *line, char **argv, int maxArgs) {
    int argc = 0;
    char *p = line;
    int inWord = 0;

    while (*p && argc < maxArgs) {
      if (*p == ' ' || *p == '\t') {
        if (inWord) {
          *p = '\0';
          inWord = 0;
        }
        p++;
      } else {
        if (!inWord) {
          argv[argc++] = p;
          inWord = 1;
        }
        p++;
      }
    }

    return argc;
  }

  const CliCommand *_commands = nullptr;
  size_t _commandCount = 0;
  const CliAlias *_aliases = nullptr;
  size_t _aliasCount = 0;
  Stream *_serial = &Serial;
  char _buf[MAX_LINE];
  size_t _len = 0;
};
