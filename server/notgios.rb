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
      pass if %w{ sign_in sign_up tasks alarms contacts }.include?(request.path_info.split('/')[1]) || request.path_info.split('/')[1].nil?

      token = request.cookies['token']
      begin
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

    post '/add_server' do
      Helpers.with_nodis do |nodis|
        starting_job = CommandStruct.new(nodis.next_id, :add, params['serverAddress'], 'TOTAL', 600, 'CPU', [])
        nodis.add_server(params['username'], starting_job, params['serverAddress'], params['serverName'], params['sshPort'])
        MIDDLEMAN.add_server(params['address'], starting_job)
      end
    end

    post '/update_server' do
      Helpers.with_nodis do |nodis|
        nodis.update_server(params['username'], params['serverAddress'], params['serverName'], params['sshPort'])
        200
      end
    end

    post '/delete_server' do
      Helpers.with_nodis do |nodis|
        nodis.delete_server(params['username'], params['address'])
        200
      end
    end

    get '/get_servers' do
      Helpers.with_nodis do |nodis|
        connected, disconnected = Array.new, Array.new
        nodis.get_servers(params['username']).each { |server| server['connected'].to_b ? connected.push(server) : disconnected.push(server) }
        {
          connectedServers: connected,
          disconnectedServers: disconnected
        }.to_json
      end
    end

    get '/get_tasks' do
      Helpers.with_nodis do |nodis|
        tasks = nodis.get_jobs_for_user(params['username']).map do |task|
          remapped = {
            id: task['id'],
            address: task['server'],
            type: task['type'].downcase,
            metric: task['metric'].downcase,
            freq: task['freq'],
            options: {}
          }
          JSON.parse(task['options']).each do |option|
            index = option.index(' ')
            key = option[0...index]
            remapped[:options][key] = option[(index + 1)..-1]
          end
          remapped
        end
        nodis.get_servers(params['username']).map do |server|
          subtasks = Array.new
          tasks.delete_if { |task| subtasks.push(task) if task[:address] == server['address'] }
          server['tasks'] = subtasks
          server
        end.to_json
      end
    end

    get '/get_metrics/:task_id' do
      Helpers.with_nodis do |nodis|
        begin
          nodis.get_recent_metrics(params['task_id'], params['username'], params['count'] || 100)
        rescue
          halt 400
        end
      end
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
