module Notgios
  class Server < Sinatra::Application

    # Sinatra Configuration
    set :port, SERVER_PORT
    set :static, true
    set :public_folder, File.join($ROOT, 'assets')
    set :bind, '0.0.0.0'

    # Enforce Authentication before handling routes.
    before do
      # Exclude the login and sign up routes.
      pass if %w{ sign_in sign_up }.include?(request.path_info.split('/')[1])

      authorization = headers 'Authorization'
      begin
        token = authorization.split(' ')[1]
        if token.exists?
          decoded = AuthToken.decode(token)
          halt 401 unless decoded.exists?
          params['username'] = decoded['username']
        else
          halt 401
        end
      rescue
        # Any kind of exception would mean authentication failed.
        halt 401
      end
    end

    # Base route.
    get %r{^/(tasks|alarms|contacts)?$} do
      erb :index
    end

    get '/get_servers/?:server?' do
      Helpers.with_nodis do |nodis|
        connected, disconnected = Array.new, Array.new
        nodis.get_servers(params['username']).each { |server| server.connected.to_b ? connected.push(server) : disconnected.push(server) }
        {
          connectedServers: connected,
          disconnectedServers: disconnected
        }.to_json
      end
    end

    get '/get_tasks/?:task?' do
      [
        {
          name: 'Stream',
          address: '104.236.124.232',
          tasks: [
            {
              id: 5,
              type: 'process',
              metric: 'cpu',
              freq: 5,
              options: {
                keepalive: true,
                pidfile: '/root/working/a.pid',
                runcmd: '/root/working/a.out'
              }
            },
            {
              id: 6,
              type: 'process',
              metric: 'memory',
              freq: 600,
              options: {
                keepalive: false,
                pidfile: '/root/working/stuff.pid',
                runcmd: '/root/working/stuff'
              }
            }
          ]
        },
        {
          name: 'Monitored',
          address: '159.203.119.88',
          tasks: [
            {
              id: 7,
              type: 'directory',
              freq: 3600,
              options: {
                path: '/root/git'
              }
            }
          ]
        }
      ].to_json
    end

    get '/get_metrics/:task_id' do

    end

    post '/sign_up' do
      halt 400 unless params[:username].exists? && params[:password].exists?

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
      halt 400 unless params[:username].exists? && params[:password].exists?

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
