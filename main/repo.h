#pragma once
#ifdef __cplusplus
extern "C" {
#endif

/* redefine throw as a whitespace */
// #define throw


struct ngn_state {
  bool initiator;
  char* buffer;
  unsigned short buffer_cap;
  void* ne;
  void* storage;
  // negentropy::storage::Vector storage;
};

struct ngn_state* ngn_init (bool initiator, char *buffer, unsigned short* io_length);
void ngn_deinit(struct ngn_state* handle);
int ngn_reconcile(struct ngn_state* handle, const unsigned short m_size, unsigned char* io_type);

#ifdef __cplusplus
}
#endif
