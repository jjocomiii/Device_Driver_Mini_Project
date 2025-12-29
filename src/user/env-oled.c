#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/rtc.h>

#define OLED_W 128
#define OLED_H 64
#define FB_SZ  (OLED_W * OLED_H / 8)

static uint8_t fb[FB_SZ];

static inline void fb_clear(void){ memset(fb, 0x00, sizeof(fb)); }
static inline void set_px(int x,int y,int on){
  if(x<0||x>=OLED_W||y<0||y>=OLED_H) return;
  int idx = x + (y/8)*OLED_W;
  uint8_t m = (uint8_t)(1u << (y%8));
  if(on) fb[idx] |= m; else fb[idx] &= (uint8_t)~m;
}
static inline void invert_rect(int x,int y,int w,int h){
  for(int yy=y; yy<y+h; yy++){
    for(int xx=x; xx<x+w; xx++){
      if(xx<0||xx>=OLED_W||yy<0||yy>=OLED_H) continue;
      int idx = xx + (yy/8)*OLED_W;
      fb[idx] ^= (uint8_t)(1u << (yy%8));
    }
  }
}

typedef struct { char ch; uint8_t col[5]; } Glyph;
static const Glyph font[] = {
  {' ',{0,0,0,0,0}},
  {'0',{0x3E,0x51,0x49,0x45,0x3E}},
  {'1',{0x00,0x42,0x7F,0x40,0x00}},
  {'2',{0x42,0x61,0x51,0x49,0x46}},
  {'3',{0x21,0x41,0x45,0x4B,0x31}},
  {'4',{0x18,0x14,0x12,0x7F,0x10}},
  {'5',{0x27,0x45,0x45,0x45,0x39}},
  {'6',{0x3C,0x4A,0x49,0x49,0x30}},
  {'7',{0x01,0x71,0x09,0x05,0x03}},
  {'8',{0x36,0x49,0x49,0x49,0x36}},
  {'9',{0x06,0x49,0x49,0x29,0x1E}},
  {'-',{0x08,0x08,0x08,0x08,0x08}},
  {':',{0x00,0x36,0x36,0x00,0x00}},
  {'%',{0x62,0x64,0x08,0x13,0x23}},
  {'C',{0x3E,0x41,0x41,0x41,0x22}},
  {'T',{0x01,0x01,0x7F,0x01,0x01}},
  {'H',{0x7F,0x08,0x08,0x08,0x7F}},
  {'E',{0x7F,0x49,0x49,0x49,0x41}},
  {'X',{0x63,0x14,0x08,0x14,0x63}},
  {'I',{0x00,0x41,0x7F,0x41,0x00}},
  {'O',{0x3E,0x41,0x41,0x41,0x3E}},
  {'N',{0x7F,0x04,0x08,0x10,0x7F}},
  {'R',{0x7F,0x09,0x19,0x29,0x46}},
  {'P',{0x7F,0x09,0x09,0x09,0x06}},
  {'A',{0x7E,0x11,0x11,0x11,0x7E}},
  {'G',{0x3E,0x41,0x49,0x49,0x3A}},
  {'D',{0x7F,0x41,0x41,0x22,0x1C}},
  {'M',{0x7F,0x02,0x04,0x02,0x7F}},
  {'Y',{0x07,0x08,0x70,0x08,0x07}},
  {'S',{0x46,0x49,0x49,0x49,0x31}},
  {'V',{0x1F,0x20,0x40,0x20,0x1F}},
  {'F',{0x7F,0x09,0x09,0x09,0x01}},
  {'L',{0x7F,0x40,0x40,0x40,0x40}},
};

static const uint8_t* glyph(char c){
  for(size_t i=0;i<sizeof(font)/sizeof(font[0]);i++)
    if(font[i].ch==c) return font[i].col;
  return font[0].col;
}
static void draw_char(int x,int y,char c,int scale){
  const uint8_t *g = glyph(c);
  for(int col=0; col<5; col++){
    uint8_t bits = g[col];
    for(int row=0; row<7; row++){
      if(((bits>>row)&1)==0) continue;
      for(int sy=0; sy<scale; sy++)
        for(int sx=0; sx<scale; sx++)
          set_px(x + col*scale + sx, y + row*scale + sy, 1);
    }
  }
}
static void draw_text(int x,int y,const char* s,int scale){
  int cx=x;
  for(size_t i=0; s[i]; i++){
    draw_char(cx,y,s[i],scale);
    cx += (5+1)*scale;
    if(cx >= OLED_W) break;
  }
}
static int fb_flush(int fd){
  ssize_t n = write(fd, fb, FB_SZ);
  return (n == FB_SZ) ? 0 : -1;
}

// -------- RTC helpers --------
static int rtc_read_raw(struct rtc_time *rt){
  int fd = open("/dev/rtc0", O_RDONLY | O_CLOEXEC);
  if(fd < 0) return -1;
  int r = ioctl(fd, RTC_RD_TIME, rt);
  int e = errno;
  close(fd);
  errno = e;
  return r;
}

static int rtc_sane(const struct rtc_time *rt){
  int Y = rt->tm_year + 1900;
  if(Y < 2000 || Y > 2099) return 0;
  if(rt->tm_mon < 0 || rt->tm_mon > 11) return 0;
  if(rt->tm_mday < 1 || rt->tm_mday > 31) return 0;
  if(rt->tm_hour < 0 || rt->tm_hour > 23) return 0;
  if(rt->tm_min < 0 || rt->tm_min > 59) return 0;
  if(rt->tm_sec < 0 || rt->tm_sec > 59) return 0;
  return 1;
}

static void rtc_from_system(struct rtc_time *rt){
  time_t now = time(NULL);
  struct tm tmv;
  localtime_r(&now, &tmv);
  rt->tm_year = tmv.tm_year;
  rt->tm_mon  = tmv.tm_mon;
  rt->tm_mday = tmv.tm_mday;
  rt->tm_hour = tmv.tm_hour;
  rt->tm_min  = tmv.tm_min;
  rt->tm_sec  = tmv.tm_sec;
}

static void rtc_add_seconds(struct rtc_time *rt, int sec){
  struct tm tmv;
  memset(&tmv, 0, sizeof(tmv));
  tmv.tm_year = rt->tm_year;
  tmv.tm_mon  = rt->tm_mon;
  tmv.tm_mday = rt->tm_mday;
  tmv.tm_hour = rt->tm_hour;
  tmv.tm_min  = rt->tm_min;
  tmv.tm_sec  = rt->tm_sec;

  time_t t = mktime(&tmv);
  if(t == (time_t)-1) return;

  t += sec;
  localtime_r(&t, &tmv);

  rt->tm_year = tmv.tm_year;
  rt->tm_mon  = tmv.tm_mon;
  rt->tm_mday = tmv.tm_mday;
  rt->tm_hour = tmv.tm_hour;
  rt->tm_min  = tmv.tm_min;
  rt->tm_sec  = tmv.tm_sec;
}


static int rtc_set_with_retry(const struct rtc_time *rt_new){
  for(int i=0;i<20;i++){
    int fd = open("/dev/rtc0", O_RDWR | O_CLOEXEC);
    if(fd >= 0){
      int r = ioctl(fd, RTC_SET_TIME, rt_new);
      int e = errno;
      close(fd);
      errno = e;
      return r;
    }
    usleep(100 * 1000);
  }
  return -1;
}

// -------- DHT (open/read/close; 너는 이미 OK라 했으니 유지) --------
static int read_file_once(const char *path, char *buf, size_t cap){
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if(fd < 0) return -1;
  int n = (int)read(fd, buf, cap-1);
  int e = errno;
  close(fd);
  errno = e;
  if(n <= 0) return -1;
  buf[n] = 0;
  return n;
}
static int parse_two_ints(const char *s, int *a, int *b){
  int cnt=0, v[2]={0,0};
  const char *p=s;
  while(*p && cnt<2){
    if((*p>='0' && *p<='9') || (*p=='-' && p[1]>='0' && p[1]<='9')){
      char *end=NULL;
      long x = strtol(p, &end, 10);
      if(end && end!=p){
        v[cnt++] = (int)x;
        p = end;
        continue;
      }
    }
    p++;
  }
  if(cnt==2){ *a=v[0]; *b=v[1]; return 0; }
  return -1;
}
static int dht_read_now(int *temp, int *humi){
  char buf[96];
  if(read_file_once("/dev/dht11", buf, sizeof(buf)) < 0) return -1;
  int a=0,b=0;
  if(parse_two_ints(buf, &a, &b) == 0){
    *temp = a; *humi = b;
    return 0;
  }
  return -1;
}

// -------- edit clamp --------
static int days_in_month(int y, int m){
  static const int d[12]={31,28,31,30,31,30,31,31,30,31,30,31};
  int leap = ((y%4==0 && y%100!=0) || (y%400==0));
  int r=d[m-1]; if(m==2 && leap) r++;
  return r;
}
static void clamp_date(int *y,int *mo,int *da){
  if(*y<2000) *y=2000; if(*y>2099) *y=2099;
  if(*mo<1) *mo=12; if(*mo>12) *mo=1;
  int dim=days_in_month(*y,*mo);
  if(*da<1) *da=dim; if(*da>dim) *da=1;
}
static void clamp_hms(int *h,int *m,int *s){
  if(*h<0) *h=23; if(*h>23) *h=0;
  if(*m<0) *m=59; if(*m>59) *m=0;
  if(*s<0) *s=59; if(*s>59) *s=0;
}

static int read_rotary_event(int fd, int *is_key, int *delta){
  char buf[128];
  int n = (int)read(fd, buf, sizeof(buf)-1);
  if(n<=0) return 0;
  buf[n]=0;

  if(strchr(buf,'K')){ *is_key=1; *delta=0; return 1; }

  if(buf[0]=='R'){
    int d=0; long t=0;
    if(sscanf(buf,"R %d %ld",&d,&t)>=1){ *is_key=0; *delta=d; return 1; }
  }

  char *p = strstr(buf,"step=");
  if(p){
    int d=0;
    if(sscanf(p,"step=%d",&d)==1){ *is_key=0; *delta=d; return 1; }
  }
  return 0;
}

enum Page { PAGE_CLOCK=0, PAGE_SENSOR=1 };
enum Field { F_YEAR=0, F_MON, F_DAY, F_HOUR, F_MIN, F_SEC, F_EXIT };

static void render_clock_view(const struct rtc_time *rt, const char *toast){
  char date[32], time_s[32];
  int Y=rt->tm_year+1900, M=rt->tm_mon+1, D=rt->tm_mday;
  int h=rt->tm_hour, m=rt->tm_min, s=rt->tm_sec;

  snprintf(date,sizeof(date),"%04d-%02d-%02d",Y,M,D);
  snprintf(time_s,sizeof(time_s),"%02d:%02d:%02d",h,m,s);

  fb_clear();
  draw_text(0, 0, date, 1);
  draw_text(0, 16, time_s, 2);
  draw_text(0, 52, toast ? toast : "K:EDIT  R:PAGE", 1);
}

static void render_clock_edit(int y,int mo,int d,int h,int mi,int s, enum Field f){
  char date[32], time_s[32];
  snprintf(date,sizeof(date),"%04d-%02d-%02d",y,mo,d);
  snprintf(time_s,sizeof(time_s),"%02d:%02d:%02d",h,mi,s);

  fb_clear();
  draw_text(0, 0, date, 1);
  draw_text(0, 16, time_s, 2);

  if(f==F_YEAR) invert_rect(0,0, 24, 9);
  if(f==F_MON)  invert_rect(30,0, 12, 9);
  if(f==F_DAY)  invert_rect(48,0, 12, 9);

  if(f==F_HOUR) invert_rect(0,16, 24, 18);
  if(f==F_MIN)  invert_rect(36,16, 24, 18);
  if(f==F_SEC)  invert_rect(72,16, 24, 18);

  if(f==F_EXIT){
    draw_text(0, 52, "EXIT", 2);
    invert_rect(0,52, 48, 18);
    draw_text(54, 52, "K:SAVE", 1);
  } else {
    draw_text(0, 52, "K:NEXT  R:CHANGE", 1);
  }
}

static void render_sensor_big(int temp,int humi,int ok){
  fb_clear();
  if(!ok){
    draw_text(0, 0, "DHT ERR", 2);
    draw_text(0, 28, "R:PAGE  K:CLOCK", 1);
    return;
  }
  char hbuf[16], tbuf[16];
  snprintf(hbuf,sizeof(hbuf),"%02d%%", humi);
  snprintf(tbuf,sizeof(tbuf),"%02dC", temp);

  draw_text(0, 0,  "HUMI", 1);
  draw_text(0, 10, hbuf, 3);
  draw_text(0, 38, "TEMP", 1);
  draw_text(0, 48, tbuf, 2);
}

int main(void){
  int fd_oled = open("/dev/ssd1306", O_WRONLY|O_CLOEXEC);
  int fd_rot  = open("/dev/rotary",  O_RDONLY|O_CLOEXEC);
  if(fd_oled<0){ perror("open /dev/ssd1306"); return 1; }
  if(fd_rot <0){ perror("open /dev/rotary");  return 1; }

  enum Page page = PAGE_CLOCK;
  int edit = 0;
  enum Field field = F_YEAR;

  // last good rtc cache
  struct rtc_time rt={0};
  struct rtc_time rt_good={0};
  int have_good = 0;

  // edit buffer
  int ey=2025, emo=1, ed=1, eh=0, emin=0, es=0;

  // sensor
  int temp=0, humi=0, dht_ok=0;

  // toast
  char toast[32]={0};
  int toast_ticks=0;

  // fallback tick (RTC read fail 시에도 화면 시간이 멈추지 않게)
  struct timespec mono_prev;
  clock_gettime(CLOCK_MONOTONIC, &mono_prev);
  long acc_ms = 0;

  while(1){
    struct pollfd pfd={.fd=fd_rot,.events=POLLIN};
    int pr = poll(&pfd,1,200);    // RTC read + sanity (RTC 실패 시 NTP(system time)로 덮지 말고, 마지막 값에서 tick)
    struct timespec mono_now;
    clock_gettime(CLOCK_MONOTONIC, &mono_now);
    long dms = (mono_now.tv_sec - mono_prev.tv_sec)*1000L +
               (mono_now.tv_nsec - mono_prev.tv_nsec)/1000000L;
    if(dms < 0) dms = 0;
    if(dms > 5000) dms = 5000;
    mono_prev = mono_now;
    acc_ms += dms;

    if(rtc_read_raw(&rt)==0 && rtc_sane(&rt)){
      rt_good = rt;
      have_good = 1;
      acc_ms = 0; // RTC로 동기화됐으니 tick 누적 초기화
    } else {
      if(!have_good){
        // 첫 초기화만 system time 사용
        rtc_from_system(&rt_good);
        have_good = 1;
        acc_ms = 0;
      } else {
        // 마지막 good 값에서 계속 진행 (NTP로 덮어쓰지 않음)
        while(acc_ms >= 1000){
          rtc_add_seconds(&rt_good, 1);
          acc_ms -= 1000;
        }
      }
    }

    // DHT
    dht_ok = (dht_read_now(&temp,&humi)==0);

    if(toast_ticks>0){
      toast_ticks--;
      if(toast_ticks==0) toast[0]=0;
    }

    // event
    if(pr>0 && (pfd.revents & POLLIN)){
      int is_key=0, delta=0;
      if(read_rotary_event(fd_rot,&is_key,&delta)){
        if(is_key){
          if(!edit){
            if(page==PAGE_CLOCK){
              // enter edit: ALWAYS from good cache
              ey   = rt_good.tm_year + 1900;
              emo  = rt_good.tm_mon + 1;
              ed   = rt_good.tm_mday;
              eh   = rt_good.tm_hour;
              emin = rt_good.tm_min;
              es   = rt_good.tm_sec;

              clamp_date(&ey,&emo,&ed);
              clamp_hms(&eh,&emin,&es);

              field = F_YEAR;
              edit = 1;
            } else {
              page = PAGE_CLOCK;
            }
          } else {
            if(field==F_EXIT){
              // ALWAYS clamp before save
              clamp_date(&ey,&emo,&ed);
              clamp_hms(&eh,&emin,&es);

              struct rtc_time nrt;
              memset(&nrt,0,sizeof(nrt));
              nrt.tm_year = ey - 1900;
              nrt.tm_mon  = emo - 1;
              nrt.tm_mday = ed;
              nrt.tm_hour = eh;
              nrt.tm_min  = emin;
              nrt.tm_sec  = es;

              if(rtc_set_with_retry(&nrt) == 0){
                snprintf(toast,sizeof(toast),"SAVED");
                toast_ticks = 10;
                // 저장 직후 cache도 즉시 갱신(다음 화면에서 바로 반영)
                rt_good = nrt;
                have_good = 1;
                clock_gettime(CLOCK_MONOTONIC, &mono_prev);
                acc_ms = 0;
              } else {
                snprintf(toast,sizeof(toast),"SAVE FAIL");
                toast_ticks = 15;
              }

              edit = 0;
              field = F_YEAR;
            } else {
              field = (enum Field)((int)field + 1);
              if(field > F_EXIT) field = F_EXIT;
            }
          }
        } else if(delta != 0){
          if(!edit){
            page = (page==PAGE_CLOCK) ? PAGE_SENSOR : PAGE_CLOCK;
          } else {
            int step = (delta>0)? +1 : -1;
            int reps = (delta>0)? delta : -delta;
            if(reps<1) reps=1;
            if(reps>5) reps=5;

            for(int k=0;k<reps;k++){
              switch(field){
                case F_YEAR: ey += step; break;
                case F_MON:  emo += step; break;
                case F_DAY:  ed += step; break;
                case F_HOUR: eh += step; break;
                case F_MIN:  emin += step; break;
                case F_SEC:  es += step; break;
                case F_EXIT: break;
              }
              clamp_date(&ey,&emo,&ed);
              clamp_hms(&eh,&emin,&es);
            }
          }
        }
      }
    }

    // render (use rt_good always)
    if(page==PAGE_CLOCK){
      if(!edit) render_clock_view(&rt_good, (toast[0]?toast:NULL));
      else      render_clock_edit(ey,emo,ed,eh,emin,es,field);
    } else {
      render_sensor_big(temp,humi,dht_ok);
    }

    (void)fb_flush(fd_oled);
  }
  return 0;
}


