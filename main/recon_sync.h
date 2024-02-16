#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include "pwire.h"
struct ngn_state {
  int initiator;
  char* buffer;
  unsigned short buffer_cap;
  void* ne;
  void* storage;
  // negentropy::storage::Vector storage;
};

struct ngn_state* ngn_init (int initiator, char *buffer, unsigned short* io_length);
void ngn_deinit(struct ngn_state* handle);
int ngn_reconcile(struct ngn_state* handle, const unsigned short m_size, unsigned char* io_type);

#ifdef __cplusplus
}
#endif
