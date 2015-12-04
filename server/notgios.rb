module Notgios
  class Server < Sinatra::Application

    # Sinatra Configuration
    set :port, SERVER_PORT
    set :static, true
    set :public_folder, File.join($ROOT, 'assets')
    set :bind, '0.0.0.0'

    # Base route.
    get(/\/|\/servers|\/tasks|\/alarms|\/contacts/) do
      erb :index
    end

    post '/sign_up' do
      with_nodis do |nodis|
        begin
          nodis.add_user(params[:username], params[:password])
          AuthToken.encode({ username: params[:username] })
        rescue Nodis::ResourceExistsError
          halt 400
        end
      end
    end

    post '/sign_in' do
      with_nodis do |nodis|
        begin
          if nodis.authenticate_user(params[:username], params[:password])
            AuthToken.encode({ username: params[:username] })
          else
            halt 401
          end
        rescue NoSuchResourceError
          halt 400
        end
      end
    end

  end
end
