#include "push_notification.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN

void PushNotificationController::send(const drogon::HttpRequestPtr& req, std::function<void (const drogon::HttpResponsePtr&)>&& callback)
{
   // 1. React가 등록해서 SQLite3 DB에 저장해둔 특정 유저의 endpoint 주소와 auth 토큰을 읽어옵니다.
   std::string userEndpoint = "https://fcm.googleapis.com/fcm/send/eXX... "; 
        
   // 2. 푸시 서버로 보낼 JSON 페이로드 작성
   Json::Value pushBody;
   pushBody["title"] = "🚨 WaveAI 건강 브리핑";
   pushBody["body"] = "수면 중 심한 코골이가 3회 이상 감지되어 가습기를 가동했습니다.";
   pushBody["url"] = "/dashboard/chat"; // 클릭 시 이동할 리액트 라우터 주소

   // 3. Drogon 내장 비동기 HttpClient를 사용하여 Google 푸시 서버 엔드포인트로 암호화 발송
   auto client = drogon::HttpClient::newHttpClient(userEndpoint);
   auto pushReq = drogon::HttpRequest::newHttpRequest();
   pushReq->setMethod(drogon::Post);
   pushReq->setBody(pushBody.toStyledString());
   
   // *주의: 실제 전송 시에는 헤더에 VAPID 암호화 서명(Authorization)을 동봉해야 크로뮴이 수용합니다.
   pushReq->addHeader("Authorization", "WebPush YOUR_VAPID_SIGNED_JWT");
   pushReq->addHeader("TTL", "60");

   client->sendRequest(pushReq, [callback](drogon::ReqResult result, const drogon::HttpResponsePtr& resp) {
       auto res = drogon::HttpResponse::newHttpResponse();
       if (result == drogon::ReqResult::Ok && resp->statusCode() == 201) {
           res->setBody("푸시 알림 발송 성공");
       } else {
           res->setBody("푸시 알림 발송 실패");
       }
       callback(res);
   });
}

WEB_NAMESPACE_END
WAVE_NAMESPACE_END