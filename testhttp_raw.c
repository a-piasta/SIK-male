#include <sys/types.h>
#include <sys/socket.h>

#include <netdb.h>
#include <unistd.h>

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "err.h"

int main(int argc, char *argv[]) {
  int sock, err;
  struct addrinfo addr_hints, *addr_result;

  if (argc != 4) {
    fatal("Usage: %s <address>:<port> <cookie file> <tested address>\n", argv[0]);
  }

  memset(&addr_hints, 0, sizeof(struct addrinfo));
  addr_hints.ai_family = AF_INET;
  addr_hints.ai_socktype = SOCK_STREAM;
  addr_hints.ai_protocol = IPPROTO_TCP;

  // znajdujemy dwukropek w adresie
  char *port = NULL;
  for (size_t i = strlen(argv[1]); i-- > 0; ) {
    if (argv[1][i] == ':') {
      argv[1][i] = 0;
      port = argv[1] + i + 1;
      break;
    }
  }

  if (port == NULL) fatal("no port specified");
  err = getaddrinfo(argv[1], port, &addr_hints, &addr_result);
  if (err == EAI_SYSTEM) { // błąd systemowy
    syserr("getaddrinfo: %s", gai_strerror(err));
  } else if (err != 0) { // inny błąd
    fatal("getaddrinfo: %s", gai_strerror(err));
  }

  // tworzenie gniazda, łączenie się
  sock = socket(addr_result->ai_family, addr_result->ai_socktype, addr_result->ai_protocol);
  if (sock < 0) syserr("socket");

  if (connect(sock, addr_result->ai_addr, addr_result->ai_addrlen) < 0) {
    syserr("connect");
  }

  freeaddrinfo(addr_result);

  FILE *sock_file = fdopen(sock, "r+");
  if (!sock_file) {
    if (errno == ENOMEM) fatal("fdopen (%d; %s)", errno, strerror(errno));
    else syserr("fdopen");
  }

  FILE *cookie_file = fopen(argv[2], "r");
  if (!cookie_file) {
    fclose(sock_file);
    syserr("opening cookie file");
  }

  char *tested_addr = argv[3];
  size_t addr_len = strlen(tested_addr);

  // sprawdzamy, czy adres zaczyna się od http czy https
  if (strncasecmp(tested_addr, "http://", 7) == 0) {
    for (size_t i = 7; i <= addr_len; ++i) tested_addr[i - 7] = tested_addr[i];
    addr_len -= 7;
  } else {
    if (strncasecmp(tested_addr, "https://", 8) == 0) {
      for (size_t i = 8; i <= addr_len; ++i) tested_addr[i - 8] = tested_addr[i];
      addr_len -= 8;
    }
  }

  size_t hostname_len = addr_len;
  bool root = true;
  // oddzielamy hosta od reszty adresu
  for (size_t i = 0; i < addr_len; ++i) {
    if (tested_addr[i] == '/' || tested_addr[i] == '#') {
      hostname_len = i;
      if (tested_addr[i] == '#') {
        tested_addr[i] = 0;
        addr_len = i;
      } else {
        root = false;
      }
      break;
    }
    if (tested_addr[i] == '?') {
      hostname_len = i;
      break;
    }
    if (tested_addr[i] == ':') tested_addr[i] = 0;
  }

  // jeżeli w adresie jest znak '#', to części za nim nie wysyłamy w zapytaniu
  for (size_t i = hostname_len; i < addr_len; ++i) {
    if (tested_addr[i] == '#') {
      tested_addr[i] = 0;
      addr_len = i;
      break;
    }
  }

  // wysyłamy zapytanie
  if (hostname_len == addr_len) {
    if (fprintf(sock_file, "GET / HTTP/1.1\r\n") < 0) fatal("fprintf");
  } else {
    int err;
    if (root) err = fprintf(sock_file, "GET /%s HTTP/1.1\r\n", tested_addr + hostname_len);
    else err = fprintf(sock_file, "GET %s HTTP/1.1\r\n", tested_addr + hostname_len);
    if (err < 0) fatal("fprintf");
    tested_addr[hostname_len] = 0;
  }
  
  if (fprintf(sock_file, "Host: %s\r\n", tested_addr) < 0) fatal("fprintf");

  // wysyłamy ciasteczka
  char *buffer = NULL;
  size_t n = 0;
  for (size_t line = 0; ; ++line) {
    ssize_t len = getline(&buffer, &n, cookie_file);
    if (len < 0) {
      if (errno == 0) goto eof_cookie;
      free(buffer);
      if (fclose(sock_file) != 0 || fclose(cookie_file) != 0) syserr("fclose");
      if (errno == ENOMEM) fatal("out of memory");
      else syserr("getline");
    }
    if (len == 0) {
      eof_cookie:
      if (line > 0) {
        if (fprintf(sock_file, "\r\n") < 0) fatal("fprintf");
      }
      break;
    }
    if (len > 0) {
      if (buffer[len - 1] == '\n') {
        if (len > 1 && buffer[len - 2] == '\r') len -= 2;
        else len--;
        buffer[len] = 0;
      }
    }
    if (line == 0) {
      if (fprintf(sock_file, "Cookie: %s", buffer) < 0) fatal("fprintf");
    } else {
      if (fprintf(sock_file, "; %s", buffer) < 0) fatal("fprintf");
    }
  }
  if (fclose(cookie_file) != 0) syserr("fclose");

  if (fprintf(sock_file, "Connection: close\r\n\r\n") < 0) fatal("fprintf");
  if (fflush(sock_file) != 0) syserr("fflush");

  // koniec zapytania; teraz odczytujemy odpowiedź

  size_t content_length = 0;
  bool header = true;
  bool encoding_chunked = false;
  bool must_read_CRLF = false;
  long expected_chunk_size = 0;

  for (size_t line = 0; ; ++line) {
    ssize_t len = getline(&buffer, &n, sock_file);
    if (len < 0) { // błąd przy wczytywaniu lub koniec pliku
      if (errno == 0) {
        if (line == 0) goto eof_no_response;
        else goto eof_sock;
      }
      free(buffer);
      if (fclose(sock_file) != 0) syserr("fclose");
      syserr("getline");
    }
    if (line == 0) { // status odpowiedzi
      if (len == 0) {
        eof_no_response:
        free(buffer);
        if (fclose(sock_file) != 0) syserr("fclose");
        fatal("no response");
      }

      // akceptujemy tylko HTTP/1.1
      if (strncmp(buffer, "HTTP/1.1 ", 9) != 0) {
        free(buffer);
        if (fclose(sock_file) != 0) syserr("fclose");
        fatal("wrong response status");
      }

      // jeżeli status jest inny niż 200 OK, to wypisujemy go i kończymy program
      if (strcmp(buffer, "HTTP/1.1 200 OK\r\n") != 0) {
        printf("%s", buffer);
        free(buffer);
        if (fclose(sock_file) != 0) syserr("fclose");

        exit(EXIT_SUCCESS);
      }
    } else {
      eof_sock:
      if (len <= 0) break; // zawsze errno = 0; wartość len < 0
        // jest wychwycona wcześniej, a skaczemy tutaj zależnie od errno
      if (!header) {
        if (encoding_chunked) {
          if (expected_chunk_size == 0 && !must_read_CRLF) {
            if (len <= 2 || strcmp(buffer + len - 2, "\r\n") != 0) {
              fatal("error at reading chunk length");
            }
            for (size_t i = len - 2; i-- > 0; ) {
              if (!isxdigit(buffer[i])) {
                fatal("chunk length should be hexadecimal number");
              }
            }
            expected_chunk_size = strtol(buffer, NULL, 16);
            if (errno != 0) syserr("strtol");
            content_length += expected_chunk_size;
            if (expected_chunk_size == 0) break;
          } else {
            if (len <= expected_chunk_size) {
              expected_chunk_size -= len;
              if (expected_chunk_size == 0) must_read_CRLF = true;
            } else {
              if (len == expected_chunk_size + 2 && strcmp(buffer + len - 2, "\r\n") == 0) {
                expected_chunk_size = 0;
                must_read_CRLF = false;
              } else {
                fatal("chunk size different than expected");
              }
            }
          }
        } else {
          content_length += len;
        }
      } else {
        if (strncasecmp(buffer, "Set-Cookie: ", 12) == 0) {
          for (ssize_t i = 12; i < len; ++i) {
            if (buffer[i] >= 0 && buffer[i] < ' ' && buffer[i] != '\r') {
              fatal("forbidden characters");
            } else if (buffer[i] == ';' || buffer[i] == '\r') {
              buffer[i] = 0;
              if (printf("%s\n", buffer + 12) < 0) fatal("printf");
              break;
            }
          }
        } if (strcasecmp(buffer, "Transfer-encoding: chunked\r\n") == 0) {
          encoding_chunked = true;
        } else {
          if (strcmp(buffer, "\r\n") == 0) header = false;
        }
      }
    }
  }

  free(buffer);
  if (fclose(sock_file) != 0) syserr("fclose");

  if (printf("Dlugosc zasobu: %zu\n", content_length) < 0) fatal("printf");

  exit(EXIT_SUCCESS);
}
