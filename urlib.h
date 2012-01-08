#ifndef URLIB_H_
#define URLIB_H_

#define URLADER "urlader"
#define URLADER_VERSION "1.0" /* a decimal number, not a version string */

enum
{
  T_NULL, // 5
  T_META, // 1 : exe_id, exe_ver
  T_ENV,  // 2 : name, value
  T_ARG,  // 3 : arg
  T_DIR,  // 4+: path
  T_FILE, // 4+: path, data
  T_NUM
};

enum
{
  F_LZF  = 0x01,
  F_EXEC = 0x10,
  F_NULL = 0
};

#define TAIL_MAGIC "ScHmOrp_PaCk_000"

struct u_pack_hdr
{
  unsigned char type;
  unsigned char flags;
  unsigned char namelen[2];
  unsigned char datalen[4];
};

#define u_16(ptr) (((ptr)[0] << 8) | (ptr)[1])
#define u_32(ptr) (((ptr)[0] << 24) | ((ptr)[1] << 16) | ((ptr)[2] << 8) | (ptr)[3])

struct u_pack_tail {
  unsigned char max_uncompressed[4]; /* maximum uncompressed file size */
  unsigned char size[4]; /* how many bytes to seke backwards from end(!) of tail */
  unsigned char reserved[8]; /* must be 0 */
  char magic[16];
  char md5_head[16]; /* md5(urlader) or 0, if there is no checksum */
  char md5_pack[16]; /* md5(pack) or 0, if there is no checksum */
};

#endif

