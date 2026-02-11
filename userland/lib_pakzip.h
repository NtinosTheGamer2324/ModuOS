#pragma once

int pakzip_extract(const char *zip_path, const char *dest_dir);
int pakzip_create_from_dir(const char *src_dir, const char *out_zip_path);
