module Notgios
  class Server < Sinatra::Application

    # Sinatra Configuration
    set :port, SERVER_PORT
    set :static, true
    set :public_folder, File.join($ROOT, 'assets')
    set :bind, '0.0.0.0'

    # Base route.
    get %r{^/(tasks|alarms|contacts)?$} do
      erb :index
    end

    get '/servers/?:server?' do
      {
        connectedServers: [
          {
            name: 'Stream',
            address: '104.236.124.232',
            lastSeen: 1449282827
          }
        ],
        disconnectedServers: [
          {
            name: 'Monitored',
            address: '159.203.119.88',
            lastSeen: 1449282827
          }
        ]
      }.to_json
    end

    post '/sign_up' do
      Helpers.with_nodis do |nodis|
        begin
          nodis.add_user(params[:username], params[:password])
          AuthToken.encode({ username: params[:username] })
        rescue Nodis::ResourceExistsError
          halt 400
        end
      end
    end

    post '/sign_in' do
      Helpers.with_nodis do |nodis|
        begin
          if nodis.authenticate_user(params[:username], params[:password])
            AuthToken.encode({ username: params[:username] })
          else
            halt 401
          end
        rescue Nodis::NoSuchResourceError
          halt 400
        end
      end
    end

  end
end
