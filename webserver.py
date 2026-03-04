#!/usr/bin/env python3
"""
간단한 HTTP 서버 - HLS 스트림 및 웹 플레이어 제공
CORS 헤더 포함하여 크로스 오리진 요청 허용
"""

import http.server
import socketserver
import os

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PORT = 8080
HLS_DIR = os.path.join("/tmp", "hls_output")  # /tmp에 생성하여 프로젝트 디렉터리 오염 방지
WEB_ROOT = SCRIPT_DIR

class CORSRequestHandler(http.server.SimpleHTTPRequestHandler):
    """CORS를 지원하는 HTTP 요청 핸들러"""
    
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=WEB_ROOT, **kwargs)
    
    def end_headers(self):
        # CORS 헤더 추가 (모든 오리진 허용)
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET, OPTIONS')
        self.send_header('Access-Control-Allow-Headers', 'Content-Type')
        
        # HLS 파일에 대한 캐시 비활성화 (실시간 스트리밍)
        if self.path.endswith('.m3u8') or self.path.endswith('.ts'):
            self.send_header('Cache-Control', 'no-cache, no-store, must-revalidate')
            self.send_header('Pragma', 'no-cache')
            self.send_header('Expires', '0')
        
        super().end_headers()
    
    def do_OPTIONS(self):
        """CORS preflight 요청 처리"""
        self.send_response(200)
        self.end_headers()

if __name__ == "__main__":
    # HLS 출력 디렉토리 생성
    os.makedirs(HLS_DIR, exist_ok=True)
    
    with socketserver.TCPServer(("", PORT), CORSRequestHandler) as httpd:
        print(f"🌐 Web server running at http://0.0.0.0:{PORT}")
        print(f"📺 Video player: http://localhost:{PORT}/viewer.html")
        print(f"📂 Serving files from: {WEB_ROOT}")
        print(f"🎬 HLS stream: http://localhost:{PORT}/hls_output/stream.m3u8")
        print("\nPress Ctrl+C to stop")
        
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\n🛑 Server stopped")
