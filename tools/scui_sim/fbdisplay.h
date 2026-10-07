#pragma once
#include <string>
#include <vector>
#include <cstdio>
#include "Adafruit_GFX.h"
class FBDisplay : public Adafruit_GFX {
public:
  int W0, H0; std::vector<uint8_t> fb;
  int updates=0, partials=0;
  FBDisplay(int w, int h) : Adafruit_GFX(w,h), W0(w), H0(h), fb(w*h,255) {}
  void drawPixel(int16_t x, int16_t y, uint16_t c) override {
    if (x<0||y<0||x>=width()||y>=height()) return;
    int px=x, py=y;
    switch (getRotation()) { case 1: std::swap(px,py); px=W0-px-1; break;
      case 2: px=W0-px-1; py=H0-py-1; break; case 3: std::swap(px,py); py=H0-py-1; break; }
    fb[py*W0+px] = c ? 255 : 0;
  }
  using Adafruit_GFX::print;
  void print(const std::string& s){ for (unsigned char ch : s) write(ch); }
  void print(const char c){ write(uint8_t(c)); }
  void update(){ updates++; }
  void updateWindow(int16_t,int16_t,int16_t,int16_t){ partials++; }
  // save in logical (rotated) orientation
  void save(const char* path){
    FILE* f=fopen(path,"wb"); int w=width(), h=height();
    fprintf(f,"P5\n%d %d\n255\n",w,h);
    for(int y=0;y<h;y++) for(int x=0;x<w;x++){
      int px=x, py=y;
      switch (getRotation()) { case 1: std::swap(px,py); px=W0-px-1; break;
        case 2: px=W0-px-1; py=H0-py-1; break; case 3: std::swap(px,py); py=H0-py-1; break; }
      fputc(fb[py*W0+px],f);
    }
    fclose(f);
  }
};
