#include <Poco/Net/HTTPBasicCredentials.h>

#include <DB/Interpreters/executeQuery.h>
#include <DB/IO/WriteBufferFromHTTPServerResponse.h>
#include <DB/IO/WriteHelpers.h>
#include <DB/IO/ReadBufferFromIStream.h>
#include <DB/IO/ReadBufferFromString.h>

#include "OLAPQueryParser.h"
#include "OLAPQueryConverter.h"

#include "OLAPHTTPHandler.h"

#include <statdaemons/Stopwatch.h>
#include <iomanip>



namespace DB
{
	void OLAPHTTPHandler::processQuery(Poco::Net::HTTPServerRequest & request, Poco::Net::HTTPServerResponse & response)
	{
		HTMLForm params(request);
		
		std::ostringstream request_ostream;
		request_ostream << request.stream().rdbuf();
		std::string request_string = request_ostream.str();

		LOG_TRACE(log, "Request URI: " << request.getURI());
		LOG_TRACE(log, "Request body: " << request_string);

		std::istringstream request_istream(request_string);
			
		BlockInputStreamPtr query_plan;

		/// Имя пользователя и пароль могут быть заданы как в параметрах URL, так и с помощью HTTP Basic authentification (и то, и другое не секъюрно).
		std::string user = params.get("user", "default");
		std::string password = params.get("password", "");
		std::string quota_key = params.get("quota_key", "");

		if (request.hasCredentials())
		{
			Poco::Net::HTTPBasicCredentials credentials(request);

			user = credentials.getUsername();
			password = credentials.getPassword();
		}
		
		Context context = *server.global_context;
		context.setGlobalContext(*server.global_context);

		context.setUser(user, password, request.clientAddress().host(), quota_key);
		
		OLAP::QueryParseResult olap_query = server.olap_parser->parse(request_istream);
		
		std::string clickhouse_query;
		server.olap_converter->OLAPServerQueryToClickhouse(olap_query, context, clickhouse_query);
		
		LOG_TRACE(log, "Converted query: " << clickhouse_query);
		
		ReadBufferFromString in(clickhouse_query);
		WriteBufferFromHTTPServerResponse out(response);
		
		Stopwatch watch;
		executeQuery(in, out, context, query_plan);
		watch.stop();
		
		if (query_plan)
		{
			std::stringstream log_str;
			log_str << "Query pipeline:\n";
			query_plan->dumpTree(log_str);
			LOG_DEBUG(log, log_str.str());
			
			/// Выведем информацию о том, сколько считано строк и байт.
			size_t rows = 0;
			size_t bytes = 0;
			
			query_plan->getLeafRowsBytes(rows, bytes);
			
			if (rows != 0)
			{
				LOG_INFO(log, std::fixed << std::setprecision(3)
				<< "Read " << rows << " rows, " << bytes / 1048576.0 << " MiB in " << watch.elapsedSeconds() << " sec., "
				<< static_cast<size_t>(rows / watch.elapsedSeconds()) << " rows/sec., " << bytes / 1048576.0 / watch.elapsedSeconds() << " MiB/sec.");
			}
		}

		QuotaForIntervals & quota = context.getQuota();
		if (!quota.empty())
			LOG_INFO(log, "Quota:\n" << quota.toString());
	}


	void OLAPHTTPHandler::handleRequest(Poco::Net::HTTPServerRequest & request, Poco::Net::HTTPServerResponse & response)
	{
		/// Для того, чтобы работал keep-alive.
		if (request.getVersion() == Poco::Net::HTTPServerRequest::HTTP_1_1)
			response.setChunkedTransferEncoding(true);

		try
		{
			processQuery(request, response);
			LOG_INFO(log, "Done processing query");
		}
		catch (Exception & e)
		{
			response.setStatusAndReason(Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
			std::stringstream s;
			s << "Code: " << e.code()
				<< ", e.displayText() = " << e.displayText() << ", e.what() = " << e.what();
			if (!response.sent())
				response.send() << s.str() << std::endl;
			LOG_ERROR(log, s.str());
		}
		catch (Poco::Exception & e)
		{
			response.setStatusAndReason(Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
			std::stringstream s;
			s << "Code: " << ErrorCodes::POCO_EXCEPTION << ", e.code() = " << e.code()
				<< ", e.displayText() = " << e.displayText() << ", e.what() = " << e.what();
			if (!response.sent())
				response.send() << s.str() << std::endl;
			LOG_ERROR(log, s.str());
		}
		catch (std::exception & e)
		{
			response.setStatusAndReason(Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
			std::stringstream s;
			s << "Code: " << ErrorCodes::STD_EXCEPTION << ". " << e.what();
			if (!response.sent())
				response.send() << s.str() << std::endl;
			LOG_ERROR(log, s.str());
		}
		catch (...)
		{
			response.setStatusAndReason(Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
			std::stringstream s;
			s << "Code: " << ErrorCodes::UNKNOWN_EXCEPTION << ". Unknown exception.";
			if (!response.sent())
				response.send() << s.str() << std::endl;
			LOG_ERROR(log, s.str());
		}
	}
}
