/**
 * dsm_client.h - CXL shim header
 * 
 * 這個檔案解決 include path 的問題：
 * 
 * Tree.h 裡寫的是 #include "dsm_client.h"
 * 原版的 dsm_client.h 在 include/ 目錄下
 * 
 * 我們在 include/cxl/ 下放一個同名的 dsm_client.h，
 * CMake 設定 include/cxl 優先於 include，
 * 所以編譯器會先找到這個 shim，然後被重定向到 CXL 版本。
 * 
 * 這就是為什麼分離目錄比 #ifdef 更乾淨的原因：
 * 只要切換 include path 的優先順序，就能切換整個實現。
 */
#pragma once
#include "dsm_client_cxl.h"
