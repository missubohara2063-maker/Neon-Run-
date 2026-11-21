#include <windows.h>
#include <windowsx.h>
#include <mmsystem.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cctype>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#if defined(_MSC_VER)
#pragma comment(lib, "winmm.lib")
#endif

namespace {
constexpr int W=960,H=720, ROAD_L=250,ROAD_R=710, CAR_W=54,CAR_H=88;
constexpr int LANE_COUNT=4, LANE_WIDTH=(ROAD_R-ROAD_L)/LANE_COUNT;
constexpr int LANE_CAR_X[LANE_COUNT]={281,396,511,626};
enum class Screen { Menu, Instructions, Playing, Paused, GameOver, Scores };
struct Traffic { float x,y; int lane; COLORREF color; };
struct Entry { std::string name; int score; };
struct SoundVoice { std::vector<int16_t> samples; WAVEHDR header{}; bool prepared=false; };
enum class SoundCue { Ui, Start, Steer, Pass, Crash, Pause, Resume, Record };
HWND windowHandle{}; Screen screen=Screen::Menu; std::string player="RACER";
HDC backBuffer{}; HBITMAP backBitmap{}; HGDIOBJ previousBitmap{};
std::filesystem::path scoreFile;
float carX=LANE_CAR_X[1], roadOffset=0, spawnClock=0, elapsed=0,scoreProgress=0; int score=0, best=0;
bool soundOn=true, leftDown=false,rightDown=false,editingName=false,ignoreNameChar=false; DWORD lastTick=0; std::vector<Traffic> traffic; std::vector<Entry> leaderboard;
std::vector<unsigned char> musicWave;
HWAVEOUT soundDevice{};std::array<SoundVoice,8> soundVoices{};DWORD lastSteerSound=0,lastPassSound=0;
COLORREF ink=RGB(15,25,35), muted=RGB(105,120,133), paper=RGB(245,248,247), teal=RGB(25,189,159), orange=RGB(255,139,86);

void put32(std::vector<unsigned char>&v,uint32_t x){for(int i=0;i<4;i++)v.push_back((unsigned char)(x>>(i*8)));}
void put16(std::vector<unsigned char>&v,uint16_t x){v.push_back((unsigned char)x);v.push_back((unsigned char)(x>>8));}
void startMusic(){
 if(!soundOn)return;
 if(musicWave.empty()){
  constexpr int rate=22050,seconds=8;musicWave.reserve(44+rate*seconds);auto&v=musicWave;
  auto tag=[&](const char*s){for(int i=0;i<4;i++)v.push_back((unsigned char)s[i]);};
  tag("RIFF");put32(v,36+rate*seconds);tag("WAVE");tag("fmt ");put32(v,16);put16(v,1);put16(v,1);put32(v,rate);put32(v,rate);put16(v,1);put16(v,8);tag("data");put32(v,rate*seconds);
  const double notes[]={110.0,130.81,164.81,146.83,110.0,130.81,196.0,164.81};
  for(int n=0;n<rate*seconds;n++){int beat=(n/rate)%8;double phase=2.0*3.141592653589793*notes[beat]*n/rate;double pulse=std::sin(phase)+.35*std::sin(phase*.5);double beatPos=(n%(rate))/double(rate);double env=std::min(1.0,beatPos*12.0)*std::max(.15,1.0-beatPos*.8);int sample=(int)(128+pulse*7*env);v.push_back((unsigned char)std::clamp(sample,0,255));}
 }
 PlaySoundW((LPCWSTR)musicWave.data(),nullptr,SND_MEMORY|SND_LOOP|SND_ASYNC|SND_NODEFAULT);
}
void initializeSoundDevice(){
 WAVEFORMATEX format{};format.wFormatTag=WAVE_FORMAT_PCM;format.nChannels=1;format.nSamplesPerSec=22050;
 format.wBitsPerSample=16;format.nBlockAlign=2;format.nAvgBytesPerSec=format.nSamplesPerSec*format.nBlockAlign;
 if(waveOutOpen(&soundDevice,WAVE_MAPPER,&format,0,0,CALLBACK_NULL)!=MMSYSERR_NOERROR)soundDevice=nullptr;
}
void releaseFinishedSounds(){
 if(!soundDevice)return;
 for(auto&voice:soundVoices)if(voice.prepared&&(voice.header.dwFlags&WHDR_DONE)){
  if(waveOutUnprepareHeader(soundDevice,&voice.header,sizeof(WAVEHDR))==MMSYSERR_NOERROR){voice.prepared=false;voice.samples.clear();voice.header={};}
 }
}
void playCue(SoundCue cue){
 if(!soundOn)return;
 DWORD now=GetTickCount();
 if(cue==SoundCue::Steer&&now-lastSteerSound<140)return;
 if(cue==SoundCue::Pass&&now-lastPassSound<450)return;
 if(cue==SoundCue::Steer)lastSteerSound=now;
 if(cue==SoundCue::Pass)lastPassSound=now;
 const double* notes=nullptr;size_t noteCount=0;int duration=80;double volume=.18;bool noise=false;
 static const double ui[]={690};static const double start[]={440,660,880};static const double steer[]={520};
 static const double pass[]={740,988};static const double crash[]={260,175,105};static const double pause[]={540,360};
 static const double resume[]={360,540,740};static const double record[]={523,659,784,1047};
 switch(cue){
  case SoundCue::Ui:notes=ui;noteCount=1;duration=55;break;
  case SoundCue::Start:notes=start;noteCount=3;duration=180;volume=.2;break;
  case SoundCue::Steer:notes=steer;noteCount=1;duration=38;volume=.11;break;
  case SoundCue::Pass:notes=pass;noteCount=2;duration=110;volume=.15;break;
  case SoundCue::Crash:notes=crash;noteCount=3;duration=260;volume=.28;noise=true;break;
  case SoundCue::Pause:notes=pause;noteCount=2;duration=110;volume=.16;break;
  case SoundCue::Resume:notes=resume;noteCount=3;duration=145;volume=.18;break;
  case SoundCue::Record:notes=record;noteCount=4;duration=300;volume=.2;break;
 }
 releaseFinishedSounds();
 if(!soundDevice){MessageBeep(cue==SoundCue::Crash?MB_ICONHAND:MB_OK);return;}
 SoundVoice* slot=nullptr;for(auto&voice:soundVoices)if(!voice.prepared){slot=&voice;break;}
 if(!slot)return;
 constexpr int sampleRate=22050;size_t count=(size_t)sampleRate*duration/1000;slot->samples.resize(count);
 double phase=0;size_t samplesPerNote=std::max<size_t>(1,count/noteCount);
 for(size_t i=0;i<count;i++){
  double t=(double)i/sampleRate,remaining=(double)(count-i)/sampleRate;
  double envelope=std::min({1.0,t/.008,remaining/.035});double frequency=notes[std::min(noteCount-1,i/samplesPerNote)];
  phase+=frequency/sampleRate;double tone=std::sin(phase*6.283185307179586)+.16*std::sin(phase*12.566370614359172);
  if(noise){uint32_t n=(uint32_t)i*1103515245u+12345u;tone=.62*tone+.38*((((n>>16)&32767)/16383.5)-1.0);}
  slot->samples[i]=(int16_t)std::clamp(tone*envelope*volume*32767.0,-32767.0,32767.0);
 }
 slot->header={};slot->header.lpData=(LPSTR)slot->samples.data();slot->header.dwBufferLength=(DWORD)(slot->samples.size()*sizeof(int16_t));
 if(waveOutPrepareHeader(soundDevice,&slot->header,sizeof(WAVEHDR))!=MMSYSERR_NOERROR){slot->samples.clear();MessageBeep(MB_OK);return;}
 slot->prepared=true;
 if(waveOutWrite(soundDevice,&slot->header,sizeof(WAVEHDR))!=MMSYSERR_NOERROR){waveOutUnprepareHeader(soundDevice,&slot->header,sizeof(WAVEHDR));slot->prepared=false;slot->samples.clear();}
}
void closeSoundDevice(){
 if(!soundDevice)return;
 waveOutReset(soundDevice);
 for(auto&voice:soundVoices)if(voice.prepared){waveOutUnprepareHeader(soundDevice,&voice.header,sizeof(WAVEHDR));voice.prepared=false;voice.samples.clear();}
 waveOutClose(soundDevice);soundDevice=nullptr;
}
std::string cleanName(std::string s){ if(s.empty()) return "RACER"; for(char& c:s) if(c=='|'||c=='\n'||c=='\r') c=' '; if(s.size()>14)s.resize(14); return s; }
void loadScores(){ leaderboard.clear(); std::ifstream f(scoreFile.empty()?std::filesystem::path(L"highscores.txt"):scoreFile); std::string line; while(std::getline(f,line)){auto p=line.find('|');if(p==std::string::npos)continue;try{leaderboard.push_back({line.substr(0,p),std::stoi(line.substr(p+1))});}catch(...){}} std::sort(leaderboard.begin(),leaderboard.end(),[](auto&a,auto&b){return a.score>b.score;});if(leaderboard.size()>8)leaderboard.resize(8);best=leaderboard.empty()?0:leaderboard.front().score; }
void saveScore(){bool newRecord=score>best;leaderboard.push_back({cleanName(player),score});std::sort(leaderboard.begin(),leaderboard.end(),[](auto&a,auto&b){return a.score>b.score;});if(leaderboard.size()>8)leaderboard.resize(8);std::ofstream f(scoreFile.empty()?std::filesystem::path(L"highscores.txt"):scoreFile,std::ios::trunc);for(auto&e:leaderboard)f<<e.name<<'|'<<e.score<<'\n';loadScores();if(newRecord)playCue(SoundCue::Record);}
void resetGame(){carX=LANE_CAR_X[1];score=0;scoreProgress=0;elapsed=spawnClock=roadOffset=0;traffic.clear();leftDown=rightDown=false;lastTick=GetTickCount();screen=Screen::Playing;startMusic();playCue(SoundCue::Start);}
HFONT setFont(int size,bool bold=false){return CreateFontW(size,0,0,0,bold?FW_BOLD:FW_MEDIUM,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");}
void fill(HDC dc,int x,int y,int w,int h,COLORREF c,int radius=0){HBRUSH b=CreateSolidBrush(c);HPEN p=CreatePen(PS_NULL,0,c);auto ob=SelectObject(dc,b),op=SelectObject(dc,p);if(radius)RoundRect(dc,x,y,x+w,y+h,radius,radius);else Rectangle(dc,x,y,x+w,y+h);SelectObject(dc,ob);SelectObject(dc,op);DeleteObject(b);DeleteObject(p);}
void text(HDC dc,int x,int y,const std::wstring&s,int size,COLORREF c,bool bold=false){HFONT f=setFont(size,bold),old=(HFONT)SelectObject(dc,f);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,c);TextOutW(dc,x,y,s.c_str(),(int)s.size());SelectObject(dc,old);DeleteObject(f);}
std::wstring ws(const std::string&s){return std::wstring(s.begin(),s.end());}
void center(HDC dc,int y,const std::wstring&s,int size,COLORREF c,bool bold=false){HFONT f=setFont(size,bold),old=(HFONT)SelectObject(dc,f);SIZE z{};GetTextExtentPoint32W(dc,s.c_str(),(int)s.size(),&z);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,c);TextOutW(dc,(W-z.cx)/2,y,s.c_str(),(int)s.size());SelectObject(dc,old);DeleteObject(f);}
void car(HDC dc,int x,int y,COLORREF c,bool playerCar=false){fill(dc,x+4,y+4,CAR_W-8,CAR_H-8,RGB(15,25,35),18);fill(dc,x,y,CAR_W,CAR_H,c,17);fill(dc,x+9,y+17,CAR_W-18,28,playerCar?RGB(180,245,231):RGB(205,225,234),11);fill(dc,x+8,y+57,CAR_W-16,18,playerCar?RGB(16,115,103):RGB(40,55,65),8);fill(dc,x-3,y+17,7,19,RGB(25,30,38),3);fill(dc,x+CAR_W-4,y+17,7,19,RGB(25,30,38),3);fill(dc,x-3,y+58,7,19,RGB(25,30,38),3);fill(dc,x+CAR_W-4,y+58,7,19,RGB(25,30,38),3);fill(dc,x+8,y+3,10,6,RGB(255,237,177),3);fill(dc,x+CAR_W-18,y+3,10,6,RGB(255,237,177),3);}
void button(HDC dc,int y,const std::wstring&label,const std::wstring&hint){fill(dc,300,y,360,58,RGB(255,255,255),15);text(dc,330,y+15,label,21,ink,true);text(dc,520,y+19,hint,13,muted);}
void drawTrack(HDC dc){fill(dc,0,0,W,H,RGB(239,246,241));fill(dc,0,0,ROAD_L,H,RGB(234,242,234));fill(dc,ROAD_R,0,W-ROAD_R,H,RGB(234,242,234));fill(dc,ROAD_L,0,ROAD_R-ROAD_L,H,RGB(52,63,72));fill(dc,ROAD_L,0,6,H,orange);fill(dc,ROAD_R-6,0,6,H,orange);for(int lane=1;lane<LANE_COUNT;lane++){int x=ROAD_L+lane*LANE_WIDTH;for(int y=-80+(int)roadOffset%120;y<H;y+=120)fill(dc,x-3,y,6,62,RGB(195,204,207));}for(int y=50;y<H;y+=135){fill(dc,47,y,52,52,RGB(210,230,218),26);fill(dc,64,y+10,18,18,RGB(152,190,167),9);}}
void tick(){
 if(screen!=Screen::Playing)return;
 DWORD now=GetTickCount();float dt=std::min(.05f,(now-lastTick)/1000.0f);lastTick=now;
 elapsed+=dt;float speed=270+std::min(260.0f,elapsed*5.0f);roadOffset+=speed*dt;
 if(leftDown)carX-=320*dt;if(rightDown)carX+=320*dt;
 carX=std::clamp(carX,(float)ROAD_L+8,(float)ROAD_R-CAR_W-8);
 scoreProgress+=dt*(12+elapsed*.35f);int earned=(int)scoreProgress;score+=earned;scoreProgress-=earned;
 spawnClock+=dt;
 if(spawnClock>std::max(.42f,1.15f-elapsed*.015f)){
  spawnClock=0;int firstLane=rand()%LANE_COUNT,chosenLane=-1;
  for(int offset=0;offset<LANE_COUNT;offset++){
   int lane=(firstLane+offset)%LANE_COUNT;
   bool laneClear=std::none_of(traffic.begin(),traffic.end(),[lane](const Traffic&t){return t.lane==lane&&t.y<12.0f&&t.y<=H;});
   if(laneClear){chosenLane=lane;break;}
  }
  if(chosenLane>=0)traffic.push_back({(float)LANE_CAR_X[chosenLane],-100,chosenLane,(rand()%2)?RGB(255,139,86):RGB(110,170,239)});
 }
 // All traffic in a lane shares the same forward motion. Combined with the
 // spawn clearance above, this keeps safe gaps from shrinking into overlaps.
 float trafficFlow=130.0f+speed*.28f;
 for(auto&t:traffic)t.y+=trafficFlow*dt;
 // The visible tyres extend three pixels past the body. Include them in both
 // hitboxes so a wheel-to-wheel collision cannot be rendered as a safe pass.
 RECT playerRect{(LONG)carX-3,560,(LONG)carX+CAR_W+3,560+CAR_H};
 for(const auto&t:traffic){
  if(t.y>H)continue;
  RECT trafficRect{(LONG)t.x-3,(LONG)t.y,(LONG)t.x+CAR_W+3,(LONG)t.y+CAR_H},intersection{};
  if(IntersectRect(&intersection,&playerRect,&trafficRect)){
   playCue(SoundCue::Crash);saveScore();screen=Screen::GameOver;traffic.clear();leftDown=rightDown=false;InvalidateRect(windowHandle,nullptr,FALSE);return;
  }
 }
 bool passed=false;
 for(const auto&t:traffic)if(t.y>H){score+=25;passed=true;}
 traffic.erase(std::remove_if(traffic.begin(),traffic.end(),[](const Traffic&t){return t.y>H;}),traffic.end());
 if(passed)playCue(SoundCue::Pass);
 InvalidateRect(windowHandle,nullptr,FALSE);
}
void draw(HDC dc){fill(dc,0,0,W,H,paper);
if(screen==Screen::Playing||screen==Screen::Paused||screen==Screen::GameOver){drawTrack(dc);for(auto&t:traffic)car(dc,(int)t.x,(int)t.y,t.color);car(dc,(int)carX,560,teal,true);fill(dc,0,0,W,78,RGB(255,255,255));text(dc,30,21,L"NEON  /  RUN",17,ink,true);text(dc,390,19,L"SCORE",12,muted,true);text(dc,390,37,ws(std::to_string(score)),22,ink,true);text(dc,540,19,L"BEST",12,muted,true);text(dc,540,37,ws(std::to_string(std::max(best,score))),22,ink,true);text(dc,760,30,L"ONE LIFE",17,orange,true);text(dc,32,673,L"←  →  STEER",13,muted,true);text(dc,783,673,L"P  PAUSE",13,muted,true);if(screen==Screen::Paused||screen==Screen::GameOver){fill(dc,0,0,W,H,RGB(15,25,35));fill(dc,275,220,410,285,RGB(250,251,249),24);center(dc,250,screen==Screen::Paused?L"TAKE A BREATHER":L"RUN COMPLETE",16,teal,true);center(dc,292,screen==Screen::Paused?L"PAUSED":L"GOOD DRIVE, RACER",36,ink,true);center(dc,351,L"SCORE   "+ws(std::to_string(score)),21,muted,true);center(dc,405,screen==Screen::Paused?L"Press P to continue":L"Press ENTER to race again",17,ink);center(dc,448,L"ESC  /  MENU",13,muted);}}
else {fill(dc,0,0,W,H,RGB(239,246,241));fill(dc,0,0,12,H,teal);fill(dc,50,48,84,34,RGB(222,240,233),16);text(dc,68,55,L"ARCADE",13,teal,true);center(dc,128,L"NEON RUN",54,ink,true);center(dc,193,L"A little less traffic. A lot more focus.",19,muted);car(dc,451,240,teal,true);
if(screen==Screen::Menu){button(dc,365,L"Start racing",L"ENTER");button(dc,435,L"How to play",L"H");button(dc,505,L"High scores",L"L");button(dc,575,L"Sound: "+std::wstring(soundOn?L"ON":L"OFF"),L"M");center(dc,650,editingName?L"TYPE NAME  ·  "+ws(player)+L"  ·  ENTER TO SAVE":L"PLAYER  ·  "+ws(player)+L"  ·  N TO EDIT",15,muted,true);}
else if(screen==Screen::Instructions){center(dc,370,L"STAY IN YOUR LANE",22,ink,true);center(dc,414,L"Arrow keys / A and D steer your car.",17,muted);center(dc,445,L"Pass traffic to score. The pace builds over time.",17,muted);center(dc,476,L"One collision ends your run.",17,muted);center(dc,542,L"ENTER  START     ESC  BACK",14,teal,true);}
else if(screen==Screen::Scores){center(dc,345,L"THE LEADERBOARD",22,ink,true);for(int i=0;i<5;i++){int rowY=386+i*35;COLORREF rowColor=i==0?teal:muted;text(dc,285,rowY,std::to_wstring(i+1)+L".",17,rowColor,i==0);if(i<(int)leaderboard.size()){text(dc,340,rowY,ws(leaderboard[i].name),17,rowColor,i==0);text(dc,620,rowY,ws(std::to_string(leaderboard[i].score)),17,rowColor,i==0);}else{text(dc,340,rowY,L"---",17,muted);text(dc,620,rowY,L"---",17,muted);}}center(dc,582,L"ENTER / ESC  BACK",14,teal,true);}}
}
void toggleSound(HWND h){soundOn=!soundOn;if(soundOn){startMusic();playCue(SoundCue::Ui);}else{PlaySoundW(nullptr,nullptr,0);if(soundDevice)waveOutReset(soundDevice);releaseFinishedSounds();}InvalidateRect(h,nullptr,FALSE);}
void showMenu(HWND h){screen=Screen::Menu;editingName=false;leftDown=rightDown=false;traffic.clear();playCue(SoundCue::Ui);InvalidateRect(h,nullptr,FALSE);}
LRESULT CALLBACK proc(HWND h,UINT m,WPARAM w,LPARAM l){
 switch(m){
  case WM_CREATE:{
   SetTimer(h,1,16,nullptr);loadScores();
   HDC windowDc=GetDC(h);backBuffer=CreateCompatibleDC(windowDc);backBitmap=CreateCompatibleBitmap(windowDc,W,H);
   if(backBuffer&&backBitmap)previousBitmap=SelectObject(backBuffer,backBitmap);
   ReleaseDC(h,windowDc);initializeSoundDevice();startMusic();return 0;
  }
  case WM_TIMER:tick();return 0;
  case WM_ERASEBKGND:return 1;
  case WM_CHAR:
   if(screen==Screen::Menu&&editingName){
    if(ignoreNameChar&&(w=='n'||w=='N')){ignoreNameChar=false;return 0;}
    ignoreNameChar=false;
    if(w==VK_RETURN){editingName=false;playCue(SoundCue::Ui);}
    else if(w==VK_BACK){if(!player.empty())player.pop_back();}
    else if(w>=32&&w<127&&player.size()<14)player.push_back((char)toupper((unsigned char)w));
    InvalidateRect(h,nullptr,FALSE);
   }
   return 0;
  case WM_KEYDOWN:{
   bool repeated=(l&(1LL<<30))!=0;
   if(screen==Screen::Playing&&(w==VK_LEFT||w=='A')){leftDown=true;if(!repeated)playCue(SoundCue::Steer);return 0;}
   if(screen==Screen::Playing&&(w==VK_RIGHT||w=='D')){rightDown=true;if(!repeated)playCue(SoundCue::Steer);return 0;}
   if(repeated)return 0;
   if(w==VK_ESCAPE){
    if(editingName){editingName=false;playCue(SoundCue::Ui);InvalidateRect(h,nullptr,FALSE);}
    else if(screen==Screen::Menu)DestroyWindow(h);
    else showMenu(h);
    return 0;
   }
   if(w==VK_RETURN){
    if(editingName){editingName=false;playCue(SoundCue::Ui);InvalidateRect(h,nullptr,FALSE);}
    else if(screen==Screen::Menu||screen==Screen::GameOver)resetGame();
    else if(screen==Screen::Instructions)resetGame();
    else if(screen==Screen::Scores)showMenu(h);
    return 0;
   }
   if(w=='N'&&screen==Screen::Menu&&!editingName){editingName=true;ignoreNameChar=true;playCue(SoundCue::Ui);InvalidateRect(h,nullptr,FALSE);return 0;}
   if(editingName)return 0;
   if(w=='M'){toggleSound(h);return 0;}
   if(w=='P'&&screen==Screen::Playing){screen=Screen::Paused;leftDown=rightDown=false;PlaySoundW(nullptr,nullptr,0);playCue(SoundCue::Pause);InvalidateRect(h,nullptr,FALSE);return 0;}
   if(w=='P'&&screen==Screen::Paused){screen=Screen::Playing;lastTick=GetTickCount();startMusic();playCue(SoundCue::Resume);InvalidateRect(h,nullptr,FALSE);return 0;}
   if(w=='H'&&screen==Screen::Menu){screen=Screen::Instructions;playCue(SoundCue::Ui);InvalidateRect(h,nullptr,FALSE);return 0;}
   if(w=='L'&&screen==Screen::Menu){loadScores();screen=Screen::Scores;playCue(SoundCue::Ui);InvalidateRect(h,nullptr,FALSE);return 0;}
   return 0;
  }
  case WM_KEYUP:
   if(w==VK_LEFT||w=='A')leftDown=false;
   if(w==VK_RIGHT||w=='D')rightDown=false;
   return 0;
  case WM_KILLFOCUS:
   leftDown=rightDown=false;
   if(screen==Screen::Playing){screen=Screen::Paused;PlaySoundW(nullptr,nullptr,0);playCue(SoundCue::Pause);}
   InvalidateRect(h,nullptr,FALSE);return 0;
  case WM_LBUTTONDOWN:{
   int x=GET_X_LPARAM(l),y=GET_Y_LPARAM(l);
   if(screen==Screen::Menu&&x>=300&&x<660){
    if(y>=365&&y<423)resetGame();
    else if(y>=435&&y<493){screen=Screen::Instructions;playCue(SoundCue::Ui);InvalidateRect(h,nullptr,FALSE);}
    else if(y>=505&&y<563){loadScores();screen=Screen::Scores;playCue(SoundCue::Ui);InvalidateRect(h,nullptr,FALSE);}
    else if(y>=575&&y<633)toggleSound(h);
    else if(y>=635){editingName=true;SetFocus(h);playCue(SoundCue::Ui);InvalidateRect(h,nullptr,FALSE);}
   }else if(screen==Screen::Instructions&&y>=520&&y<580)resetGame();
   else if(screen==Screen::Scores&&y>=560)showMenu(h);
   else if(screen==Screen::Paused&&x>=275&&x<685&&y>=220&&y<505){screen=Screen::Playing;lastTick=GetTickCount();startMusic();playCue(SoundCue::Resume);InvalidateRect(h,nullptr,FALSE);}
   else if(screen==Screen::GameOver&&x>=275&&x<685&&y>=220&&y<505)resetGame();
   return 0;
  }
  case WM_PAINT:{
   PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);HDC target=backBuffer?backBuffer:dc;draw(target);
   if(backBuffer)BitBlt(dc,0,0,W,H,backBuffer,0,0,SRCCOPY);
   EndPaint(h,&ps);return 0;
  }
  case WM_DESTROY:
   KillTimer(h,1);PlaySoundW(nullptr,nullptr,0);closeSoundDevice();
   if(backBuffer&&previousBitmap)SelectObject(backBuffer,previousBitmap);
   if(backBitmap)DeleteObject(backBitmap);
   if(backBuffer)DeleteDC(backBuffer);
   PostQuitMessage(0);return 0;
 }
 return DefWindowProcW(h,m,w,l);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int show){wchar_t exePath[MAX_PATH]{};DWORD pathLength=GetModuleFileNameW(nullptr,exePath,MAX_PATH);if(pathLength&&pathLength<MAX_PATH)scoreFile=std::filesystem::path(exePath).parent_path()/L"highscores.txt";WNDCLASSW wc{};wc.style=CS_HREDRAW|CS_VREDRAW;wc.lpfnWndProc=proc;wc.hInstance=instance;wc.lpszClassName=L"NeonRunWindow";wc.hCursor=LoadCursor(nullptr,IDC_ARROW);wc.hbrBackground=nullptr;RegisterClassW(&wc);RECT r{0,0,W,H};AdjustWindowRect(&r,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,FALSE);windowHandle=CreateWindowW(wc.lpszClassName,L"NEON RUN — 2D Car Racing | Four Lanes · One Life",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,CW_USEDEFAULT,CW_USEDEFAULT,r.right-r.left,r.bottom-r.top,nullptr,nullptr,instance,nullptr);if(!windowHandle)return 1;ShowWindow(windowHandle,show);MSG msg;while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}return (int)msg.wParam;}
